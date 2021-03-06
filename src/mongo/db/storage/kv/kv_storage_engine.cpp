/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/kv/kv_storage_engine.h"

#include <algorithm>

#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;
using std::vector;

namespace {
const std::string catalogInfo = "_mdb_catalog";
}

class KVStorageEngine::RemoveDBChange : public RecoveryUnit::Change {
public:
    RemoveDBChange(KVStorageEngine* engine, StringData db, KVDatabaseCatalogEntryBase* entry)
        : _engine(engine), _db(db.toString()), _entry(entry) {}

    virtual void commit() {
        delete _entry;
    }

    virtual void rollback() {
        stdx::lock_guard<stdx::mutex> lk(_engine->_dbsLock);
        _engine->_dbs[_db] = _entry;
    }

    KVStorageEngine* const _engine;
    const std::string _db;
    KVDatabaseCatalogEntryBase* const _entry;
};

KVStorageEngine::KVStorageEngine(
    KVEngine* engine,
    const KVStorageEngineOptions& options,
    stdx::function<KVDatabaseCatalogEntryFactory> databaseCatalogEntryFactory)
    : _databaseCatalogEntryFactory(std::move(databaseCatalogEntryFactory)),
      _options(options),
      _engine(engine),
      _supportsDocLocking(_engine->supportsDocLocking()),
      _supportsDBLocking(_engine->supportsDBLocking()) {
    uassert(28601,
            "Storage engine does not support --directoryperdb",
            !(options.directoryPerDB && !engine->supportsDirectoryPerDB()));

    OperationContextNoop opCtx(_engine->newRecoveryUnit());

    bool catalogExists = engine->hasIdent(&opCtx, catalogInfo);

    if (options.forRepair && catalogExists) {
        log() << "Repairing catalog metadata";
        // TODO should also validate all BSON in the catalog.
        engine->repairIdent(&opCtx, catalogInfo).transitional_ignore();
    }

    if (!catalogExists) {
        WriteUnitOfWork uow(&opCtx);

        Status status = _engine->createGroupedRecordStore(
            &opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);
        // BadValue is usually caused by invalid configuration string.
        // We still fassert() but without a stack trace.
        if (status.code() == ErrorCodes::BadValue) {
            fassertFailedNoTrace(28562);
        }
        fassert(28520, status);
        uow.commit();
    }

    _catalogRecordStore = _engine->getGroupedRecordStore(
        &opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);
    _catalog.reset(new KVCatalog(
        _catalogRecordStore.get(), _options.directoryPerDB, _options.directoryForIndexes));
    _catalog->init(&opCtx);

    std::vector<std::string> collections;
    _catalog->getAllCollections(&collections);

    KVPrefix maxSeenPrefix = KVPrefix::kNotPrefixed;
    for (size_t i = 0; i < collections.size(); i++) {
        std::string coll = collections[i];
        NamespaceString nss(coll);
        string dbName = nss.db().toString();

        // No rollback since this is only for committed dbs.
        KVDatabaseCatalogEntryBase*& db = _dbs[dbName];
        if (!db) {
            db = _databaseCatalogEntryFactory(dbName, this).release();
        }

        db->initCollection(&opCtx, coll, options.forRepair);
        auto maxPrefixForCollection = _catalog->getMetaData(&opCtx, coll).getMaxPrefix();
        maxSeenPrefix = std::max(maxSeenPrefix, maxPrefixForCollection);
    }

    KVPrefix::setLargestPrefix(maxSeenPrefix);
    opCtx.recoveryUnit()->abandonSnapshot();
}

/**
 * This method reconciles differences between idents the KVEngine is aware of and the
 * KVCatalog. There are three differences to consider:
 *
 * First, a KVEngine may know of an ident that the KVCatalog does not. This method will drop
 * the ident from the KVEngine.
 *
 * Second, a KVCatalog may have a collection ident that the KVEngine does not. This is an
 * illegal state and this method fasserts.
 *
 * Third, a KVCatalog may have an index ident that the KVEngine does not. This method will
 * rebuild the index.
 */
StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>>
KVStorageEngine::reconcileCatalogAndIdents(OperationContext* opCtx) {
    // Gather all tables known to the storage engine and drop those that aren't cross-referenced
    // in the _mdb_catalog. This can happen for two reasons.
    //
    // First, collection creation and deletion happen in two steps. First the storage engine
    // creates/deletes the table, followed by the change to the _mdb_catalog. It's not assumed a
    // storage engine can make these steps atomic.
    //
    // Second, a replica set node in 3.6+ on supported storage engines will only persist "stable"
    // data to disk. That is data which replication guarantees won't be rolled back. The
    // _mdb_catalog will reflect the "stable" set of collections/indexes. However, it's not
    // expected for a storage engine's ability to persist stable data to extend to "stable
    // tables".
    std::set<std::string> engineIdents;
    {
        std::vector<std::string> vec = _engine->getAllIdents(opCtx);
        engineIdents.insert(vec.begin(), vec.end());
        engineIdents.erase(catalogInfo);
    }

    std::set<std::string> catalogIdents;
    {
        std::vector<std::string> vec = _catalog->getAllIdents(opCtx);
        catalogIdents.insert(vec.begin(), vec.end());
    }

    // Drop all idents in the storage engine that are not known to the catalog. This can happen in
    // the case of a collection or index creation being rolled back.
    for (const auto& it : engineIdents) {
        if (catalogIdents.find(it) != catalogIdents.end()) {
            continue;
        }

        if (!_catalog->isUserDataIdent(it)) {
            continue;
        }

        const auto& toRemove = it;
        log() << "Dropping unknown ident: " << toRemove;
        WriteUnitOfWork wuow(opCtx);
        fassertStatusOK(40591, _engine->dropIdent(opCtx, toRemove));
        wuow.commit();
    }

    // Scan all collections in the catalog and make sure their ident is known to the storage
    // engine. An omission here is fatal. A missing ident could mean a collection drop was rolled
    // back. Note that startup already attempts to open tables; this should only catch errors in
    // other contexts such as `recoverToStableTimestamp`.
    std::vector<std::string> collections;
    _catalog->getAllCollections(&collections);
    for (const auto& coll : collections) {
        const auto& identForColl = _catalog->getCollectionIdent(coll);
        if (engineIdents.find(identForColl) == engineIdents.end()) {
            return {ErrorCodes::UnrecoverableRollbackError,
                    str::stream() << "Expected collection does not exist. NS: " << coll
                                  << " Ident: "
                                  << identForColl};
        }
    }

    // Scan all indexes and return those in the catalog where the storage engine does not have the
    // corresponding ident. The caller is expected to rebuild these indexes.
    std::vector<CollectionIndexNamePair> ret;
    for (const auto& coll : collections) {
        const BSONCollectionCatalogEntry::MetaData metaData = _catalog->getMetaData(opCtx, coll);
        for (const auto& indexMetaData : metaData.indexes) {
            const std::string& indexName = indexMetaData.name();
            std::string indexIdent = _catalog->getIndexIdent(opCtx, coll, indexName);
            if (engineIdents.find(indexIdent) != engineIdents.end()) {
                continue;
            }

            log() << "Expected index data is missing, rebuilding. NS: " << coll
                  << " Index: " << indexName << " Ident: " << indexIdent;

            ret.push_back(CollectionIndexNamePair(coll, indexName));
        }
    }

    return ret;
}

void KVStorageEngine::cleanShutdown() {
    for (DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it) {
        delete it->second;
    }
    _dbs.clear();

    _catalog.reset(NULL);
    _catalogRecordStore.reset(NULL);

    _engine->cleanShutdown();
    // intentionally not deleting _engine
}

KVStorageEngine::~KVStorageEngine() {}

void KVStorageEngine::finishInit() {}

RecoveryUnit* KVStorageEngine::newRecoveryUnit() {
    if (!_engine) {
        // shutdown
        return NULL;
    }
    return _engine->newRecoveryUnit();
}

void KVStorageEngine::listDatabases(std::vector<std::string>* out) const {
    stdx::lock_guard<stdx::mutex> lk(_dbsLock);
    for (DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it) {
        if (it->second->isEmpty())
            continue;
        out->push_back(it->first);
    }
}

KVDatabaseCatalogEntryBase* KVStorageEngine::getDatabaseCatalogEntry(OperationContext* opCtx,
                                                                     StringData dbName) {
    stdx::lock_guard<stdx::mutex> lk(_dbsLock);
    KVDatabaseCatalogEntryBase*& db = _dbs[dbName.toString()];
    if (!db) {
        // Not registering change since db creation is implicit and never rolled back.
        db = _databaseCatalogEntryFactory(dbName, this).release();
    }
    return db;
}

Status KVStorageEngine::closeDatabase(OperationContext* opCtx, StringData db) {
    // This is ok to be a no-op as there is no database layer in kv.
    return Status::OK();
}

Status KVStorageEngine::dropDatabase(OperationContext* opCtx, StringData db) {
    KVDatabaseCatalogEntryBase* entry;
    {
        stdx::lock_guard<stdx::mutex> lk(_dbsLock);
        DBMap::const_iterator it = _dbs.find(db.toString());
        if (it == _dbs.end())
            return Status(ErrorCodes::NamespaceNotFound, "db not found to drop");
        entry = it->second;
    }

    std::list<std::string> toDrop;
    entry->getCollectionNamespaces(&toDrop);

    // Partition `toDrop` into ranges of `[untimestampedCollections...,
    // timestampedCollections...]`. All timestamped collections must have already been renamed to
    // a drop-pending namespace. Running without replication treats all collections as not
    // timestamped.
    auto untimestampedDropsEnd =
        std::partition(toDrop.begin(), toDrop.end(), [](const std::string& dropNs) {
            return !NamespaceString(dropNs).isDropPendingNamespace();
        });

    // The primary caller (`DatabaseImpl::dropDatabase`) of this method currently
    // `transitional_ignore`s the result. To minimize the impact of that, while also returning a
    // correct status, attempt to drop every collection, and if there were any errors, return the
    // first one.
    Status firstError = Status::OK();

    // First drop the "non-timestamped" collections. "Non-timestamped" collections such as user
    // collections in `local` or `system.profile` do not get rolled back. This means we also
    // should not rollback their creation or deletion. To achieve that, the code takes care to
    // suppress any timestamping state.
    firstError = _dropCollectionsNoTimestamp(opCtx, entry, toDrop.begin(), untimestampedDropsEnd);

    // Now drop any leftover timestamped collections (i.e: not already dropped by the reaper).  On
    // secondaries there is already a `commit timestamp` set and these drops inherit the timestamp
    // of the `dropDatabase` oplog entry. On primaries, we look at the logical clock and set the
    // commit timestamp state.
    //
    // Additionally, before returning, this method will remove the `KVDatabaseCatalogEntry` from
    // the `_dbs` map. This action creates a new constraint that this "timestamped drop" method
    // must happen after the "non-timestamped drops".
    auto status =
        _dropCollectionsWithTimestamp(opCtx, entry, toDrop, untimestampedDropsEnd, toDrop.end());
    if (firstError.isOK()) {
        firstError = status;
    }

    return firstError;
}

/**
 * Returns the first `dropCollection` error that this method encounters. This method will attempt
 * to drop all collections, regardless of the error status.
 */
Status KVStorageEngine::_dropCollectionsNoTimestamp(OperationContext* opCtx,
                                                    KVDatabaseCatalogEntryBase* dbce,
                                                    CollIter begin,
                                                    CollIter end) {
    // On primaries, this method will be called outside of any `TimestampBlock` state meaning the
    // "commit timestamp" will not be set. For this case, this method needs no special logic to
    // avoid timestamping the upcoming writes.
    //
    // On secondaries, there will be a wrapping `TimestampBlock` and the "commit timestamp" will
    // be set. Carefully save that to the side so the following writes can go through without that
    // context.
    const Timestamp commitTs = opCtx->recoveryUnit()->getCommitTimestamp();
    if (!commitTs.isNull()) {
        opCtx->recoveryUnit()->clearCommitTimestamp();
    }

    // Ensure the method exits with the same "commit timestamp" state that it was called with.
    auto addCommitTimestamp = MakeGuard([&opCtx, commitTs] {
        if (!commitTs.isNull()) {
            opCtx->recoveryUnit()->setCommitTimestamp(commitTs);
        }
    });

    Status firstError = Status::OK();
    WriteUnitOfWork untimestampedDropWuow(opCtx);
    for (auto toDrop = begin; toDrop != end; ++toDrop) {
        std::string coll = *toDrop;
        NamespaceString nss(coll);

        // When in steady state replication and after filtering out drop-pending namespaces, the
        // only collections that may show up here are either 1) not replicated 2) `tmp.mr` 3)
        // `system.indexes`.
        //
        // Due to a bug in the `createCollection` command, `system.indexes` can become a real
        // collection in the storage engine's catalog. However, this collection is often treated
        // as a special collection. For example, dropping a database will skip over
        // `system.indexes` and it will never be renamed to the drop pending namespace.
        if (_initialDataTimestamp != Timestamp::kAllowUnstableCheckpointsSentinel) {
            invariant(!nss.isReplicated() || nss.coll().startsWith("tmp.mr") ||
                          nss.isSystemDotIndexes(),
                      str::stream() << "Collection drop is not being timestamped. Namespace: "
                                    << nss.ns());
        }

        Status result = dbce->dropCollection(opCtx, coll);
        if (!result.isOK() && firstError.isOK()) {
            firstError = result;
        }
    }

    untimestampedDropWuow.commit();
    return firstError;
}

Status KVStorageEngine::_dropCollectionsWithTimestamp(OperationContext* opCtx,
                                                      KVDatabaseCatalogEntryBase* dbce,
                                                      std::list<std::string>& toDrop,
                                                      CollIter begin,
                                                      CollIter end) {
    // On primaries, these collection drops are performed in a separate WUOW than the insertion of
    // the `dropDatabase` oplog entry. In this case, we expect the `existingCommitTs` to be null
    // and the code looks at the logical clock to assign a timestamp to the writes.
    //
    // Secondaries reach this from within a `TimestampBlock` where there should be a non-null
    // `existingCommitTs`.
    const Timestamp existingCommitTs = opCtx->recoveryUnit()->getCommitTimestamp();

    // `LogicalClock`s on standalones and master/slave do not necessarily return real
    // optimes. Assume it's safe to not timestamp the write.
    const Timestamp chosenCommitTs = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();
    const bool setCommitTs = existingCommitTs.isNull() && !chosenCommitTs.isNull();
    if (setCommitTs) {
        opCtx->recoveryUnit()->setCommitTimestamp(chosenCommitTs);
    }

    // Ensure the method exits with the same "commit timestamp" state that it was called with.
    auto removeCommitTimestamp = MakeGuard([&opCtx, setCommitTs] {
        if (setCommitTs) {
            opCtx->recoveryUnit()->clearCommitTimestamp();
        }
    });

    // This is called outside of a WUOW since MMAPv1 has unfortunate behavior around dropping
    // databases. We need to create one here since we want db dropping to all-or-nothing
    // wherever possible. Eventually we want to move this up so that it can include the logOp
    // inside of the WUOW, but that would require making DB dropping happen inside the Dur
    // system for MMAPv1.
    WriteUnitOfWork timestampedDropWuow(opCtx);

    Status firstError = Status::OK();
    for (auto toDropStr = begin; toDropStr != toDrop.end(); ++toDropStr) {
        std::string coll = *toDropStr;
        NamespaceString nss(coll);

        Status result = dbce->dropCollection(opCtx, coll);
        if (!result.isOK() && firstError.isOK()) {
            firstError = result;
        }
    }

    toDrop.clear();
    dbce->getCollectionNamespaces(&toDrop);
    invariant(toDrop.empty());

    {
        stdx::lock_guard<stdx::mutex> lk(_dbsLock);
        opCtx->recoveryUnit()->registerChange(new RemoveDBChange(this, dbce->name(), dbce));
        _dbs.erase(dbce->name());
    }

    timestampedDropWuow.commit();
    return firstError;
}

int KVStorageEngine::flushAllFiles(OperationContext* opCtx, bool sync) {
    return _engine->flushAllFiles(opCtx, sync);
}

Status KVStorageEngine::beginBackup(OperationContext* opCtx) {
    // We should not proceed if we are already in backup mode
    if (_inBackupMode)
        return Status(ErrorCodes::BadValue, "Already in Backup Mode");
    Status status = _engine->beginBackup(opCtx);
    if (status.isOK())
        _inBackupMode = true;
    return status;
}

void KVStorageEngine::endBackup(OperationContext* opCtx) {
    // We should never reach here if we aren't already in backup mode
    invariant(_inBackupMode);
    _engine->endBackup(opCtx);
    _inBackupMode = false;
}

bool KVStorageEngine::isDurable() const {
    return _engine->isDurable();
}

bool KVStorageEngine::isEphemeral() const {
    return _engine->isEphemeral();
}

SnapshotManager* KVStorageEngine::getSnapshotManager() const {
    return _engine->getSnapshotManager();
}

Status KVStorageEngine::repairRecordStore(OperationContext* opCtx, const std::string& ns) {
    Status status = _engine->repairIdent(opCtx, _catalog->getCollectionIdent(ns));
    if (!status.isOK())
        return status;

    _dbs[nsToDatabase(ns)]->reinitCollectionAfterRepair(opCtx, ns);
    return Status::OK();
}

void KVStorageEngine::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}

void KVStorageEngine::setStableTimestamp(Timestamp stableTimestamp) {
    _engine->setStableTimestamp(stableTimestamp);
}

void KVStorageEngine::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    _initialDataTimestamp = initialDataTimestamp;
    _engine->setInitialDataTimestamp(initialDataTimestamp);
}

void KVStorageEngine::setOldestTimestamp(Timestamp oldestTimestamp) {
    _engine->setOldestTimestamp(oldestTimestamp);
}

bool KVStorageEngine::supportsRecoverToStableTimestamp() const {
    return _engine->supportsRecoverToStableTimestamp();
}

Status KVStorageEngine::recoverToStableTimestamp() {
    return _engine->recoverToStableTimestamp();
}

bool KVStorageEngine::supportsReadConcernSnapshot() const {
    return _engine->supportsReadConcernSnapshot();
}

void KVStorageEngine::replicationBatchIsComplete() const {
    return _engine->replicationBatchIsComplete();
}
}  // namespace mongo
