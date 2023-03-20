/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <functional>
#include <map>
#include <set>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/views_for_database.h"
#include "mongo/db/database_name.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog_entry.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/functional.h"
#include "mongo/util/immutable/unordered_map.h"
#include "mongo/util/immutable/unordered_set.h"
#include "mongo/util/uuid.h"

namespace mongo {

class CollectionCatalog {
    friend class iterator;

public:
    using CollectionInfoFn = std::function<bool(const Collection* collection)>;

    // Number of how many Collection references for a single Collection that is stored in the
    // catalog. Used to determine whether there are external references (uniquely owned). Needs to
    // be kept in sync with the data structures below.
    static constexpr size_t kNumCollectionReferencesStored = 3;

    class iterator {
    public:
        using value_type = const Collection*;

        iterator(OperationContext* opCtx,
                 const DatabaseName& dbName,
                 const CollectionCatalog& catalog);
        iterator(OperationContext* opCtx,
                 std::map<std::pair<DatabaseName, UUID>,
                          std::shared_ptr<Collection>>::const_iterator mapIter,
                 const CollectionCatalog& catalog);
        value_type operator*();
        iterator operator++();
        iterator operator++(int);
        UUID uuid() const;

        /*
         * Equality operators == and != do not attempt to reposition the iterators being compared.
         * The behavior for comparing invalid iterators is undefined.
         */
        bool operator==(const iterator& other) const;
        bool operator!=(const iterator& other) const;

    private:
        bool _exhausted();

        OperationContext* _opCtx;
        DatabaseName _dbName;
        boost::optional<UUID> _uuid;
        std::map<std::pair<DatabaseName, UUID>, std::shared_ptr<Collection>>::const_iterator
            _mapIter;
        const CollectionCatalog* _catalog;
    };

    struct ProfileSettings {
        int level;
        std::shared_ptr<const ProfileFilter> filter;  // nullable

        ProfileSettings(int level, std::shared_ptr<ProfileFilter> filter)
            : level(level), filter(filter) {
            // ProfileSettings represents a state, not a request to change the state.
            // -1 is not a valid profiling level: it is only used in requests, to represent
            // leaving the state unchanged.
            invariant(0 <= level && level <= 2,
                      str::stream() << "Invalid profiling level: " << level);
        }

        ProfileSettings() = default;

        bool operator==(const ProfileSettings& other) const {
            return level == other.level && filter == other.filter;
        }
    };

    /**
     * Returns a CollectionCatalog instance capable of returning Collection instances consistent
     * with the storage snapshot. Is the same as latest() below if no snapshot is opened.
     *
     * Is the default method of acquiring a CollectionCatalog instance.
     */
    static std::shared_ptr<const CollectionCatalog> get(OperationContext* opCtx);

    /**
     * Returns a CollectionCatalog instance that reflects the latest state of the server.
     *
     * Used to confirm whether Collection instances are write eligiable.
     */
    static std::shared_ptr<const CollectionCatalog> latest(OperationContext* opCtx);

    /**
     * Like latest() above.
     *
     * Bypasses batched writing and should not be used in a context where there might be an ongoing
     * batched write.
     */
    static std::shared_ptr<const CollectionCatalog> latest(ServiceContext* svcCtx);

    /**
     * Stashes provided CollectionCatalog pointer on the RecoveryUnit snapshot.
     * Will cause get() to return this instance while the snapshot remains open.
     */
    static void stash(OperationContext* opCtx, std::shared_ptr<const CollectionCatalog> catalog);

    /**
     * Perform a write to the catalog using copy-on-write. A catalog previously returned by get()
     * will not be modified.
     *
     * This call will block until the modified catalog has been committed. Concurrant writes are
     * batched together and will thus block each other. It is important to not perform blocking
     * operations such as acquiring locks or waiting for I/O in the write job as that would also
     * block other writers.
     *
     * The provided job is allowed to throw which will be propagated through this call.
     *
     * The write job may execute on a different thread.
     */
    using CatalogWriteFn = std::function<void(CollectionCatalog&)>;
    static void write(ServiceContext* svcCtx, CatalogWriteFn job);
    static void write(OperationContext* opCtx, CatalogWriteFn job);

    /**
     * Create a new view 'viewName' with contents defined by running the specified aggregation
     * 'pipeline' with collation 'collation' on a collection or view 'viewOn'. May insert this view
     * into the system.views collection depending on 'durability'.
     *
     * Must be in WriteUnitOfWork. View creation rolls back if the unit of work aborts.
     *
     * Caller must ensure corresponding database exists. Expects db.system.views MODE_X lock and
     * view namespace MODE_IX lock (unless 'durability' is set to kAlreadyDurable).
     */
    Status createView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline,
                      const ViewsForDatabase::PipelineValidatorFn& validatePipeline,
                      const BSONObj& collation,
                      ViewsForDatabase::Durability durability =
                          ViewsForDatabase::Durability::kNotYetDurable) const;

    /**
     * Drop the view named 'viewName'.
     *
     * Must be in WriteUnitOfWork. The drop rolls back if the unit of work aborts.
     *
     * Caller must ensure corresponding database exists.
     */
    Status dropView(OperationContext* opCtx, const NamespaceString& viewName) const;

    /**
     * Modify the view named 'viewName' to have the new 'viewOn' and 'pipeline'.
     *
     * Must be in WriteUnitOfWork. The modification rolls back if the unit of work aborts.
     *
     * Caller must ensure corresponding database exists.
     */
    Status modifyView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline,
                      const ViewsForDatabase::PipelineValidatorFn& validatePipeline) const;

    /**
     * Reloads the in-memory state of the view catalog from the 'system.views' collection. The
     * durable view definitions will be validated. Reading stops on the first invalid entry with
     * errors logged and returned. Performs no cycle detection, etc.
     *
     * This is implicitly called by other methods when write operations are performed on the
     * view catalog, on external changes to the 'system.views' collection and on the first
     * opening of a database.
     *
     * Callers must re-fetch the catalog to observe changes.
     *
     * Requires an X lock on the 'system.views' collection'.
     */
    void reloadViews(OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Establish a collection instance consistent with the opened storage snapshot.
     *
     * Returns the collection pointer representative of 'nssOrUUID' at the provided read timestamp.
     * If no timestamp is provided, returns instance of the latest collection. When called
     * concurrently with a DDL operation the latest collection returned may be the instance being
     * committed by the concurrent DDL operation.
     *
     * Returns nullptr when reading from a point-in-time where the collection did not exist.
     *
     * The returned collection instance is only valid while a reference to this catalog instance is
     * held or stashed and as long as the storage snapshot remains open. Releasing catalog reference
     * or closing the storage snapshot invalidates the instance.
     *
     * Future calls to lookupCollection, lookupNSS, lookupUUID on this namespace/UUID will return
     * results consistent with the opened storage snapshot.
     *
     * Depending on the internal state of the CollectionCatalog a read from the durable catalog may
     * be performed and this call may block on I/O. No mutex should be held while calling this
     * function.
     *
     * Multikey state is not guaranteed to be consistent with the storage snapshot. It may indicate
     * an index to be multikey where it is not multikey in the storage snapshot. However, it will
     * never be wrong in the other direction.
     *
     * No collection level lock is required to call this function.
     */
    const Collection* establishConsistentCollection(OperationContext* opCtx,
                                                    const NamespaceStringOrUUID& nssOrUUID,
                                                    boost::optional<Timestamp> readTimestamp) const;

    /**
     * Returns a shared_ptr to a drop pending index if it's found and not expired.
     */
    std::shared_ptr<IndexCatalogEntry> findDropPendingIndex(StringData ident) const;

    /**
     * Handles committing a collection to the catalog within a WriteUnitOfWork.
     *
     * Must be called within a WriteUnitOfWork.
     */
    void onCreateCollection(OperationContext* opCtx, std::shared_ptr<Collection> coll) const;

    /**
     * This function is responsible for safely tracking a Collection rename within a
     * WriteUnitOfWork.
     *
     * Must be called within a WriteUnitOfWork.
     */
    void onCollectionRename(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& fromCollection) const;

    /**
     * Marks an index as dropped for this OperationContext. The drop will be committed into the
     * catalog on commit.
     *
     * Maintains the index in a drop pending state in the catalog until the underlying data files
     * are deleted.
     *
     * Must be called within a WriteUnitOfWork.
     */
    void dropIndex(OperationContext* opCtx,
                   const NamespaceString& nss,
                   std::shared_ptr<IndexCatalogEntry> indexEntry,
                   bool isDropPending) const;

    /**
     * Marks a collection as dropped for this OperationContext. Will cause the collection
     * to appear dropped for this OperationContext. The drop will be committed into the catalog on
     * commit.
     *
     * Maintains the collection in a drop pending state in the catalog until the underlying data
     * files are deleted.
     *
     * Must be called within a WriteUnitOfWork.
     */
    void dropCollection(OperationContext* opCtx, Collection* coll, bool isDropPending) const;

    /**
     * Removes the view records associated with 'dbName', if any, from the in-memory
     * representation of the catalog. Should be called when Database instance is closed. Requires X
     * lock on database namespace.
     */
    void onCloseDatabase(OperationContext* opCtx, DatabaseName dbName);

    /**
     * Register the collection with `uuid` at a given commitTime.
     *
     * The global lock must be held in exclusive mode.
     */
    void registerCollection(OperationContext* opCtx,
                            const UUID& uuid,
                            std::shared_ptr<Collection> collection,
                            boost::optional<Timestamp> commitTime);

    /**
     * Like 'registerCollection' above but allows the Collection to be registered using just a
     * MODE_IX lock on the namespace. The collection will be added to the catalog using a two-phase
     * commit where it is marked as 'pending commit' internally. The user must call
     * 'onCreateCollection' which sets up the necessary state for finishing the two-phase commit.
     */
    void registerCollectionTwoPhase(OperationContext* opCtx,
                                    const UUID& uuid,
                                    std::shared_ptr<Collection> collection,
                                    boost::optional<Timestamp> commitTime);

    /**
     * Deregister the collection.
     *
     * Adds the collection to the drop pending state in the catalog when isDropPending=true.
     */
    std::shared_ptr<Collection> deregisterCollection(OperationContext* opCtx,
                                                     const UUID& uuid,
                                                     bool isDropPending,
                                                     boost::optional<Timestamp> commitTime);

    /**
     * Create a temporary record of an uncommitted view namespace to aid in detecting a simultaneous
     * attempt to create a collection with the same namespace.
     */
    void registerUncommittedView(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Remove the temporary record for an uncommitted view namespace, either on commit or rollback.
     */
    void deregisterUncommittedView(const NamespaceString& nss);

    /**
     * Deregister all the collection objects and view namespaces.
     */
    void deregisterAllCollectionsAndViews(ServiceContext* svcCtx);

    /**
     * Adds the index entry to the drop pending state in the catalog.
     */
    void deregisterIndex(OperationContext* opCtx,
                         std::shared_ptr<IndexCatalogEntry> indexEntry,
                         bool isDropPending);

    /**
     * Clears the in-memory state for the views associated with a particular database.
     *
     * Callers must re-fetch the catalog to observe changes.
     */
    void clearViews(OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Notifies the collection catalog that the data files for the drop pending ident have been
     * removed from disk.
     */
    void notifyIdentDropped(const std::string& ident);

    /**
     * Returns a Collection pointer that corresponds to the provided
     * NamespaceString/UUID/NamespaceOrUUID.
     *
     * For the returned collection instance to remain valid remains valid, one of two preconditions
     * needs to be met:
     * 1. A collection lock of at least MODE_IS is being held.
     * 2. A reference to this catalog instance is held or stashed AND the storage snapshot remains
     *    open.
     *
     * Releasing the collection lock, catalog instance or storage snapshot will invalidate the
     * returned collection instance.
     *
     * A read or write AutoGetCollection style RAII object meets the requirements and ensures
     * validity for collection instances during its lifetime.
     *
     * It is NOT safe to cache this pointer or any pointer obtained from this instance across
     * storage snapshots such as query yield.
     *
     * Returns nullptr if no collection is known.
     */
    const Collection* lookupCollectionByUUID(OperationContext* opCtx, UUID uuid) const;
    const Collection* lookupCollectionByNamespace(OperationContext* opCtx,
                                                  const NamespaceString& nss) const;
    const Collection* lookupCollectionByNamespaceOrUUID(
        OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const;

    /**
     * Returns a non-const Collection pointer that corresponds to the provided NamespaceString/UUID
     * for a DDL operation.
     *
     * A MODE_X collection lock is required to call this function, unless the namespace/UUID
     * corresponds to an uncommitted collection creation in which case a MODE_IX lock is sufficient.
     *
     * A WriteUnitOfWork must be active and the instance returned will be created using
     * copy-on-write and will be different than prior calls to lookupCollection. However, subsequent
     * calls to lookupCollection will return the same instance as this function as long as the
     * WriteUnitOfWork remains active.
     *
     * When the WriteUnitOfWork commits future versions of the CollectionCatalog will return this
     * instance. If the WriteUnitOfWork rolls back the instance will be discarded.
     *
     * It is safe to write to the returned instance in onCommit handlers but not in onRollback
     * handlers.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    Collection* lookupCollectionByUUIDForMetadataWrite(OperationContext* opCtx,
                                                       const UUID& uuid) const;
    Collection* lookupCollectionByNamespaceForMetadataWrite(OperationContext* opCtx,
                                                            const NamespaceString& nss) const;

    /**
     * Returns true if the collection has been registered in the CollectionCatalog but not yet made
     * visible.
     */
    bool isCollectionAwaitingVisibility(UUID uuid) const;

    // TODO SERVER-74468: Remove this function
    std::shared_ptr<const Collection> lookupCollectionByNamespaceForRead_DONT_USE(
        OperationContext* opCtx, const NamespaceString& nss) const {
        return _getCollectionByNamespace(opCtx, nss);
    }
    // TODO SERVER-74468: Remove this function
    std::shared_ptr<const Collection> lookupCollectionByUUIDForRead_DONT_USE(
        OperationContext* opCtx, const UUID& uuid) const {
        return _getCollectionByUUID(opCtx, uuid);
    }

    /**
     * This function gets the NamespaceString from the collection catalog entry that
     * corresponds to UUID uuid. If no collection exists with the uuid, return
     * boost::none. See onCloseCatalog/onOpenCatalog for more info.
     */
    boost::optional<NamespaceString> lookupNSSByUUID(OperationContext* opCtx,
                                                     const UUID& uuid) const;

    /**
     * Returns the UUID if `nss` exists in CollectionCatalog.
     */
    boost::optional<UUID> lookupUUIDByNSS(OperationContext* opCtx,
                                          const NamespaceString& nss) const;

    /**
     * Returns true if this CollectionCatalog contains the provided collection instance
     */
    bool containsCollection(OperationContext* opCtx, const Collection* collection) const;

    /**
     * Returns the CatalogId for a given 'nss' or 'uuid' at timestamp 'ts'.
     */
    struct CatalogIdLookup {
        enum class Existence {
            // Namespace or UUID exists at time 'ts' and catalogId set in 'id'.
            kExists,
            // Namespace or UUID does not exist at time 'ts'.
            kNotExists,
            // Namespace or UUID existence at time 'ts' is unknown. The durable catalog must be
            // scanned to determine.
            kUnknown
        };
        RecordId id;
        Existence result;
    };
    CatalogIdLookup lookupCatalogIdByNSS(const NamespaceString& nss,
                                         boost::optional<Timestamp> ts = boost::none) const;
    CatalogIdLookup lookupCatalogIdByUUID(const UUID& uuid,
                                          boost::optional<Timestamp> ts = boost::none) const;

    /**
     * Iterates through the views in the catalog associated with database `dbName`, applying
     * 'callback' to each view.  If the 'callback' returns false, the iterator exits early.
     *
     * Caller must ensure corresponding database exists.
     */
    void iterateViews(OperationContext* opCtx,
                      const DatabaseName& dbName,
                      const std::function<bool(const ViewDefinition& view)>& callback) const;

    /**
     * Look up the 'nss' in the view catalog, returning a shared pointer to a View definition,
     * or nullptr if it doesn't exist.
     *
     * Caller must ensure corresponding database exists.
     */
    std::shared_ptr<const ViewDefinition> lookupView(OperationContext* opCtx,
                                                     const NamespaceString& nss) const;

    /**
     * Same functionality as above, except this function skips validating durable views in the
     * view catalog.
     *
     * Caller must ensure corresponding database exists.
     */
    std::shared_ptr<const ViewDefinition> lookupViewWithoutValidatingDurable(
        OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Without acquiring any locks resolves the given NamespaceStringOrUUID to an actual namespace.
     * Throws NamespaceNotFound if the collection UUID cannot be resolved to a name, or if the UUID
     * can be resolved, but the resulting collection is in the wrong database.
     */
    NamespaceString resolveNamespaceStringOrUUID(OperationContext* opCtx,
                                                 NamespaceStringOrUUID nsOrUUID) const;

    /**
     * Returns whether the collection with 'uuid' satisfies the provided 'predicate'. If the
     * collection with 'uuid' is not found, false is returned.
     */
    bool checkIfCollectionSatisfiable(UUID uuid, CollectionInfoFn predicate) const;

    /**
     * This function gets the UUIDs of all collections from `dbName`.
     *
     * If the caller does not take a strong database lock, some of UUIDs might no longer exist (due
     * to collection drop) after this function returns.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<UUID> getAllCollectionUUIDsFromDb(const DatabaseName& dbName) const;

    /**
     * This function gets the ns of all collections from `dbName`. The result is not sorted.
     *
     * Caller must take a strong database lock; otherwise, collections returned could be dropped or
     * renamed.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<NamespaceString> getAllCollectionNamesFromDb(OperationContext* opCtx,
                                                             const DatabaseName& dbName) const;

    /**
     * This function gets all the database names. The result is sorted in alphabetical ascending
     * order.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<DatabaseName> getAllDbNames() const;

    /**
     * This function gets all the database names associated with tenantId. The result is sorted in
     * alphabetical ascending order.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<DatabaseName> getAllDbNamesForTenant(boost::optional<TenantId> tenantId) const;

    /**
     * This function gets all tenantIds in the database in ascending order.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * Only returns tenantIds which are attached to at least one non-empty database.
     */
    std::set<TenantId> getAllTenants() const;

    /**
     * Updates the profile filter on all databases with non-default settings.
     */
    void setAllDatabaseProfileFilters(std::shared_ptr<ProfileFilter> filter);

    /**
     * Sets 'newProfileSettings' as the profiling settings for the database 'dbName'.
     */
    void setDatabaseProfileSettings(const DatabaseName& dbName, ProfileSettings newProfileSettings);

    /**
     * Fetches the profiling settings for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     */
    ProfileSettings getDatabaseProfileSettings(const DatabaseName& dbName) const;

    /**
     * Fetches the profiling level for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     *
     * There is no corresponding setDatabaseProfileLevel; use setDatabaseProfileSettings instead.
     * This method only exists as a convenience.
     */
    int getDatabaseProfileLevel(const DatabaseName& dbName) const {
        return getDatabaseProfileSettings(dbName).level;
    }

    /**
     * Clears the database profile settings entry for 'dbName'.
     */
    void clearDatabaseProfileSettings(const DatabaseName& dbName);

    /**
     * Statistics for the types of collections in the catalog.
     * Total collections = 'internal' + 'userCollections'
     */
    struct Stats {
        // Non-system collections on non-internal databases
        int userCollections = 0;
        // Non-system capped collections on non-internal databases
        int userCapped = 0;
        // Non-system clustered collection on non-internal databases.
        int userClustered = 0;
        // System collections or collections on internal databases
        int internal = 0;
    };

    /**
     * Returns statistics for the collection catalog.
     */
    Stats getStats() const;

    /**
     * Returns view statistics for the specified database.
     */
    boost::optional<ViewsForDatabase::Stats> getViewStatsForDatabase(
        OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Returns a set of databases, by name, that have view catalogs.
     */
    using ViewCatalogSet = absl::flat_hash_set<DatabaseName>;
    ViewCatalogSet getViewCatalogDbNames(OperationContext* opCtx) const;

    /**
     * Puts the catalog in closed state. In this state, the lookupNSSByUUID method will fall back to
     * the pre-close state to resolve queries for currently unknown UUIDs. This allows processes,
     * like authorization and replication, which need to do lookups outside of database locks, to
     * proceed.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onCloseCatalog();

    /**
     * Puts the catalog back in open state, removing the pre-close state. See onCloseCatalog.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onOpenCatalog();

    /**
     * The epoch is incremented whenever the catalog is closed and re-opened.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * This allows callers to detect an intervening catalog close. For example, closing the catalog
     * must kill all active queries. This is implemented by checking that the epoch has not changed
     * during query yield recovery.
     */
    uint64_t getEpoch() const;

    iterator begin(OperationContext* opCtx, const DatabaseName& dbName) const;
    iterator end(OperationContext* opCtx) const;

    /**
     * Checks if 'cleanupForOldestTimestampAdvanced' should be called when the oldest timestamp
     * advanced. Used to avoid a potentially expensive call to 'cleanupForOldestTimestampAdvanced'
     * if no write is needed.
     */
    bool needsCleanupForOldestTimestamp(Timestamp oldest) const;

    /**
     * Cleans up internal structures when the oldest timestamp advances
     */
    void cleanupForOldestTimestampAdvanced(Timestamp oldest);

    /**
     * Cleans up internal structures after catalog reopen
     */
    void cleanupForCatalogReopen(Timestamp stable);

    /**
     * Ensures we have a MODE_X lock on a collection or MODE_IX lock for newly created collections.
     */
    static void invariantHasExclusiveAccessToCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss);
    static bool hasExclusiveAccessToCollection(OperationContext* opCtx, const NamespaceString& nss);

private:
    friend class CollectionCatalog::iterator;
    class PublishCatalogUpdates;

    /**
     * Gets shared_ptr to Collections by UUID/Namespace.
     */
    std::shared_ptr<const Collection> _getCollectionByNamespace(OperationContext* opCtx,
                                                                const NamespaceString& nss) const;

    std::shared_ptr<const Collection> _getCollectionByUUID(OperationContext* opCtx,
                                                           const UUID& uuid) const;

    /**
     * Register the collection with `uuid`.
     *
     * If 'twoPhase' is true, this call must be followed by 'onCreateCollection' which continues the
     * two-phase commit process.
     */
    void _registerCollection(OperationContext* opCtx,
                             const UUID& uuid,
                             std::shared_ptr<Collection> collection,
                             bool twoPhase,
                             boost::optional<Timestamp> commitTime);

    std::shared_ptr<Collection> _lookupCollectionByUUID(UUID uuid) const;

    const Collection* _lookupSystemViews(OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Searches for a catalog entry at a point-in-time.
     */
    boost::optional<DurableCatalogEntry> _fetchPITCatalogEntry(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        boost::optional<Timestamp> readTimestamp) const;

    /**
     * Tries to create a Collection instance using existing shared collection state. Returns nullptr
     * if unable to do so.
     */
    std::shared_ptr<Collection> _createCompatibleCollection(
        OperationContext* opCtx,
        const std::shared_ptr<const Collection>& latestCollection,
        boost::optional<Timestamp> readTimestamp,
        const DurableCatalogEntry& catalogEntry) const;

    /**
     * Creates a Collection instance from scratch if the ident has not yet been dropped.
     */
    std::shared_ptr<Collection> _createNewPITCollection(
        OperationContext* opCtx,
        boost::optional<Timestamp> readTimestamp,
        const DurableCatalogEntry& catalogEntry) const;

    /**
     * Retrieves the views for a given database, including any uncommitted changes for this
     * operation.
     */
    boost::optional<const ViewsForDatabase&> _getViewsForDatabase(OperationContext* opCtx,
                                                                  const DatabaseName& dbName) const;

    /**
     * Iterates over databases, and performs a callback on each database. If any callback fails,
     * returns its error code. If tenantId is set, will iterate only over databases with that
     * tenantId. nextUpperBound is a callback that controls how we iterate -- given the current
     * database name, returns a <DatabaseName, UUID> pair which must be strictly less than the next
     * entry we iterate to.
     */
    Status _iterAllDbNamesHelper(
        const boost::optional<TenantId>& tenantId,
        const std::function<Status(const DatabaseName&)>& callback,
        const std::function<std::pair<DatabaseName, UUID>(const DatabaseName&)>& nextLowerBound)
        const;

    /**
     * Sets all namespaces used by views for a database. Will uassert if there is a conflicting
     * collection name in the catalog.
     */
    void _replaceViewsForDatabase(const DatabaseName& dbName, ViewsForDatabase&& views);

    /**
     * Returns true if this CollectionCatalog instance is part of an ongoing batched catalog write.
     */
    bool _isCatalogBatchWriter() const;

    /**
     * Returns true if we can saftely skip performing copy-on-write on the provided collection
     * instance.
     */
    bool _alreadyClonedForBatchedWriter(const std::shared_ptr<Collection>& collection) const;

    /**
     * Throws 'WriteConflictException' if given namespace is already registered with the catalog, as
     * either a view or collection. The results will include namespaces which have been registered
     * by preCommitHooks on other threads, but which have not truly been committed yet.
     *
     * If 'type' is set to 'NamespaceType::kCollection', we will only check for collisions with
     * collections. If set to 'NamespaceType::kAll', we will check against both collections and
     * views.
     */
    enum class NamespaceType { kAll, kCollection };
    void _ensureNamespaceDoesNotExist(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      NamespaceType type) const;

    /**
     * CatalogId with Timestamp
     */
    struct TimestampedCatalogId {
        boost::optional<RecordId> id;
        Timestamp ts;
    };

    // Push a catalogId for namespace and UUID at given Timestamp. Timestamp needs to be larger than
    // other entries for this namespace and UUID. boost::none for catalogId represent drop,
    // boost::none for timestamp turns this operation into a no-op.
    void _pushCatalogIdForNSSAndUUID(const NamespaceString& nss,
                                     const UUID& uuid,
                                     boost::optional<RecordId> catalogId,
                                     boost::optional<Timestamp> ts);

    // Push a catalogId for 'from' and 'to' for a rename operation at given Timestamp. Timestamp
    // needs to be larger than other entries for these namespaces. boost::none for timestamp turns
    // this operation into a no-op.
    void _pushCatalogIdForRename(const NamespaceString& from,
                                 const NamespaceString& to,
                                 boost::optional<Timestamp> ts);

    // Inserts a catalogId for namespace and UUID at given Timestamp, if not boost::none. Used after
    // scanning the durable catalog for a correct mapping at the given timestamp.
    void _insertCatalogIdForNSSAndUUIDAfterScan(boost::optional<const NamespaceString&> nss,
                                                boost::optional<UUID> uuid,
                                                boost::optional<RecordId> catalogId,
                                                Timestamp ts);

    // Helper to calculate if a namespace or UUID needs to be marked for cleanup for a set of
    // timestamped catalogIds
    template <class Key, class CatalogIdChangesContainer>
    void _markForCatalogIdCleanupIfNeeded(const Key& key,
                                          CatalogIdChangesContainer& catalogIdChangesContainer,
                                          const std::vector<TimestampedCatalogId>& ids);

    /**
     * Returns true if catalog information about this namespace or UUID should be looked up from the
     * durable catalog rather than using the in-memory state of the catalog.
     *
     * This is true when either:
     *  - The readTimestamp is prior to the minimum valid timestamp for the collection corresponding
     *    to this namespace, or
     *  - There's no read timestamp provided and this namespace has a pending DDL operation that has
     *    not completed yet (which would imply that the latest version of the catalog may or may not
     *    match the state of the durable catalog for this collection).
     */
    bool _needsOpenCollection(OperationContext* opCtx,
                              const NamespaceStringOrUUID& nsOrUUID,
                              boost::optional<Timestamp> readTimestamp) const;

    /**
     * Returns the collection pointer representative of 'nssOrUUID' at the provided read timestamp.
     * If no timestamp is provided, returns instance of the latest collection. The returned
     * collection instance is only valid while the storage snapshot is open and becomes invalidated
     * when the snapshot is closed.
     *
     * Returns nullptr when reading from a point-in-time where the collection did not exist.
     */
    const Collection* _openCollection(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nssOrUUID,
                                      boost::optional<Timestamp> readTimestamp) const;

    // Helpers to perform openCollection at latest or at point-in-time on Namespace/UUID.
    const Collection* _openCollectionAtLatestByNamespaceOrUUID(
        OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const;
    const Collection* _openCollectionAtPointInTimeByNamespaceOrUUID(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        Timestamp readTimestamp) const;

    // Helpers for 'lookupCatalogIdByNSS' and 'lookupCatalogIdByUUID'.
    CatalogIdLookup _checkWithOldestCatalogIdTimestampMaintained(
        boost::optional<Timestamp> ts) const;
    CatalogIdLookup _findCatalogIdInRange(boost::optional<Timestamp> ts,
                                          const std::vector<TimestampedCatalogId>& range) const;

    /**
     * When present, indicates that the catalog is in closed state, and contains a map from UUID
     * to pre-close NSS. See also onCloseCatalog.
     */
    boost::optional<mongo::stdx::unordered_map<UUID, NamespaceString, UUID::Hash>> _shadowCatalog;

    using CollectionCatalogMap =
        immutable::unordered_map<UUID, std::shared_ptr<Collection>, UUID::Hash>;
    using OrderedCollectionMap =
        std::map<std::pair<DatabaseName, UUID>, std::shared_ptr<Collection>>;
    using NamespaceCollectionMap =
        immutable::unordered_map<NamespaceString, std::shared_ptr<Collection>>;
    using UncommittedViewsSet = stdx::unordered_set<NamespaceString>;
    using DatabaseProfileSettingsMap = stdx::unordered_map<DatabaseName, ProfileSettings>;
    using ViewsForDatabaseMap = stdx::unordered_map<DatabaseName, ViewsForDatabase>;

    CollectionCatalogMap _catalog;
    OrderedCollectionMap _orderedCollections;  // Ordered by <dbName, collUUID> pair
    NamespaceCollectionMap _collections;
    UncommittedViewsSet _uncommittedViews;

    // Namespaces and UUIDs in pending commit. The opened storage snapshot must be consulted to
    // confirm visibility. The instance may be used if the namespace/uuid are otherwise unoccupied
    // in the CollectionCatalog.
    immutable::unordered_map<NamespaceString, std::shared_ptr<Collection>> _pendingCommitNamespaces;
    immutable::unordered_map<UUID, std::shared_ptr<Collection>, UUID::Hash> _pendingCommitUUIDs;

    // CatalogId mappings for all known namespaces and UUIDs for the CollectionCatalog. The vector
    // is sorted on timestamp. UUIDs will have at most two entries. One for the create and another
    // for the drop. UUIDs stay the same across collection renames.
    immutable::unordered_map<NamespaceString, std::vector<TimestampedCatalogId>> _nssCatalogIds;
    immutable::unordered_map<UUID, std::vector<TimestampedCatalogId>, UUID::Hash> _uuidCatalogIds;
    // Set of namespaces and UUIDs that need cleanup when the oldest timestamp advances
    // sufficiently.
    immutable::unordered_set<NamespaceString> _nssCatalogIdChanges;
    immutable::unordered_set<UUID, UUID::Hash> _uuidCatalogIdChanges;
    // Point at which the oldest timestamp need to advance for there to be any catalogId namespace
    // that can be cleaned up
    Timestamp _lowestCatalogIdTimestampForCleanup = Timestamp::max();
    // The oldest timestamp at which the catalog maintains catalogId mappings. Anything older than
    // this is unknown and must be discovered by scanning the durable catalog.
    Timestamp _oldestCatalogIdTimestampMaintained = Timestamp::max();

    // Map of database names to their corresponding views and other associated state.
    ViewsForDatabaseMap _viewsForDatabase;

    // Map of drop pending idents to their instance of Collection/IndexCatalogEntry. To avoid
    // affecting the lifetime and delay of the ident drop from the ident reaper, these need to be a
    // weak_ptr.
    StringMap<std::weak_ptr<Collection>> _dropPendingCollection;
    StringMap<std::weak_ptr<IndexCatalogEntry>> _dropPendingIndex;

    // Incremented whenever the CollectionCatalog gets closed and reopened (onCloseCatalog and
    // onOpenCatalog).
    //
    // Catalog objects are destroyed and recreated when the catalog is closed and re-opened. We
    // increment this counter to track when the catalog is reopened. This permits callers to detect
    // after yielding whether their catalog pointers are still valid. Collection UUIDs are not
    // sufficient, since they remain stable across catalog re-opening.
    //
    // A thread must hold the global exclusive lock to write to this variable, and must hold the
    // global lock in at least MODE_IS to read it.
    uint64_t _epoch = 0;

    /**
     * Contains non-default database profile settings. New collections, current collections and
     * views must all be able to access the correct profile settings for the database in which they
     * reside. Simple database name to struct ProfileSettings map.
     */
    DatabaseProfileSettingsMap _databaseProfileSettings;

    // Tracks usage of collection usage features (e.g. capped).
    Stats _stats;
};

/**
 * RAII style object to stash a versioned CollectionCatalog on the OperationContext.
 * Calls to CollectionCatalog::get(OperationContext*) will return this instance.
 *
 * Unstashes the CollectionCatalog at destruction if the OperationContext::isLockFreeReadsOp()
 * flag is no longer set. This is handling for the nested Stasher use case.
 */
class CollectionCatalogStasher {
public:
    CollectionCatalogStasher(OperationContext* opCtx);
    CollectionCatalogStasher(OperationContext* opCtx,
                             std::shared_ptr<const CollectionCatalog> catalog);

    /**
     * Unstashes the catalog if _opCtx->isLockFreeReadsOp() is no longer set.
     */
    ~CollectionCatalogStasher();

    /**
     * Moves ownership of the stash to the new instance, and marks the old one unstashed.
     */
    CollectionCatalogStasher(CollectionCatalogStasher&& other);

    CollectionCatalogStasher(const CollectionCatalogStasher&) = delete;
    CollectionCatalogStasher& operator=(const CollectionCatalogStasher&) = delete;
    CollectionCatalogStasher& operator=(CollectionCatalogStasher&&) = delete;

    /**
     * Stashes 'catalog' on the _opCtx.
     */
    void stash(std::shared_ptr<const CollectionCatalog> catalog);

    /**
     * Resets the OperationContext so CollectionCatalog::get() returns latest catalog again
     */
    void reset();

private:
    OperationContext* _opCtx;
    bool _stashed;
};

/**
 * RAII class to perform multiple writes to the CollectionCatalog on a single copy of the
 * CollectionCatalog instance. Requires the global lock to be held in exclusive write mode (MODE_X)
 * for the lifetime of this object.
 */
class BatchedCollectionCatalogWriter {
public:
    BatchedCollectionCatalogWriter(OperationContext* opCtx);
    ~BatchedCollectionCatalogWriter();

    BatchedCollectionCatalogWriter(const BatchedCollectionCatalogWriter&) = delete;
    BatchedCollectionCatalogWriter(BatchedCollectionCatalogWriter&&) = delete;

    const CollectionCatalog* operator->() const {
        return _batchedInstance;
    }

private:
    OperationContext* _opCtx;
    // Store base when we clone the CollectionCatalog so we can verify that there has been no other
    // writers during the batching.
    std::shared_ptr<CollectionCatalog> _base = nullptr;
    const CollectionCatalog* _batchedInstance = nullptr;
};

}  // namespace mongo
