/*
 * TenantManagement.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#if defined(NO_INTELLISENSE) && !defined(FDBCLIENT_TENANT_MANAGEMENT_ACTOR_G_H)
#define FDBCLIENT_TENANT_MANAGEMENT_ACTOR_G_H
#include "fdbclient/TenantManagement.actor.g.h"
#elif !defined(FDBCLIENT_TENANT_MANAGEMENT_ACTOR_H)
#define FDBCLIENT_TENANT_MANAGEMENT_ACTOR_H

#include <algorithm>
#include <string>
#include <map>
#include "fdbclient/ClientBooleanParams.h"
#include "fdbclient/GenericTransactionHelper.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/MetaclusterRegistration.h"
#include "fdbclient/SystemData.h"
#include "fdbclient/Tenant.h"
#include "flow/IRandom.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/actorcompiler.h" // has to be last include

namespace TenantAPI {

static const int TENANT_ID_PREFIX_MIN_VALUE = 0;
static const int TENANT_ID_PREFIX_MAX_VALUE = 32767;

template <class Transaction>
Future<Optional<TenantMapEntry>> tryGetTenantTransaction(Transaction tr, int64_t tenantId) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	return TenantMetadata::tenantMap().get(tr, tenantId);
}

ACTOR template <class Transaction>
Future<Optional<TenantMapEntry>> tryGetTenantTransaction(Transaction tr, TenantName name) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	Optional<int64_t> tenantId = wait(TenantMetadata::tenantNameIndex().get(tr, name));
	if (tenantId.present()) {
		Optional<TenantMapEntry> entry = wait(TenantMetadata::tenantMap().get(tr, tenantId.get()));
		return entry;
	} else {
		return Optional<TenantMapEntry>();
	}
}

ACTOR template <class DB, class Tenant>
Future<Optional<TenantMapEntry>> tryGetTenant(Reference<DB> db, Tenant tenant) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			Optional<TenantMapEntry> entry = wait(tryGetTenantTransaction(tr, tenant));
			return entry;
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

ACTOR template <class Transaction, class Tenant>
Future<TenantMapEntry> getTenantTransaction(Transaction tr, Tenant tenant) {
	Optional<TenantMapEntry> entry = wait(tryGetTenantTransaction(tr, tenant));
	if (!entry.present()) {
		throw tenant_not_found();
	}

	return entry.get();
}

ACTOR template <class DB, class Tenant>
Future<TenantMapEntry> getTenant(Reference<DB> db, Tenant tenant) {
	Optional<TenantMapEntry> entry = wait(tryGetTenant(db, tenant));
	if (!entry.present()) {
		throw tenant_not_found();
	}

	return entry.get();
}

ACTOR template <class Transaction>
Future<ClusterType> getClusterType(Transaction tr) {
	Optional<MetaclusterRegistrationEntry> metaclusterRegistration =
	    wait(metacluster::metadata::metaclusterRegistration().get(tr));

	return metaclusterRegistration.present() ? metaclusterRegistration.get().clusterType : ClusterType::STANDALONE;
}

ACTOR template <class Transaction>
Future<Void> checkTenantMode(Transaction tr, ClusterType expectedClusterType) {
	state typename transaction_future_type<Transaction, Optional<Value>>::type tenantModeFuture =
	    tr->get(configKeysPrefix.withSuffix("tenant_mode"_sr));

	state ClusterType actualClusterType = wait(getClusterType(tr));
	Optional<Value> tenantModeValue = wait(safeThreadFutureToFuture(tenantModeFuture));

	TenantMode tenantMode = TenantMode::fromValue(tenantModeValue.castTo<ValueRef>());
	if (actualClusterType != expectedClusterType) {
		CODE_PROBE(true, "Attempting tenant operation on wrong cluster type");
		throw invalid_metacluster_operation();
	} else if (actualClusterType == ClusterType::STANDALONE && tenantMode == TenantMode::DISABLED) {
		CODE_PROBE(true, "Attempting tenant operation on cluster with tenants disabled", probe::decoration::rare);
		throw tenants_disabled();
	}

	return Void();
}

TenantMode tenantModeForClusterType(ClusterType clusterType, TenantMode tenantMode);
int64_t extractTenantIdFromMutation(MutationRef m);
int64_t extractTenantIdFromKeyRef(StringRef s);
bool tenantMapChanging(MutationRef const& mutation, KeyRangeRef const& tenantMapRange);
int64_t computeNextTenantId(int64_t tenantId, int64_t delta);
int64_t getMaxAllowableTenantId(int64_t curTenantId);
int64_t getTenantIdPrefix(int64_t tenantId);

ACTOR template <class Transaction>
Future<TenantMode> getEffectiveTenantMode(Transaction tr) {
	state typename transaction_future_type<Transaction, Optional<Value>>::type tenantModeFuture =
	    tr->get(configKeysPrefix.withSuffix("tenant_mode"_sr));
	state ClusterType clusterType;
	state Optional<Value> tenantModeValue;
	wait(store(clusterType, getClusterType(tr)) && store(tenantModeValue, safeThreadFutureToFuture(tenantModeFuture)));
	TenantMode tenantMode = TenantMode::fromValue(tenantModeValue.castTo<ValueRef>());
	return tenantModeForClusterType(clusterType, tenantMode);
}

// Returns true if the specified ID has already been deleted and false if not. If the ID is old enough
// that we no longer keep tombstones for it, an error is thrown.
ACTOR template <class Transaction>
Future<bool> checkTombstone(Transaction tr, int64_t id) {
	state Future<bool> tombstoneFuture = TenantMetadata::tenantTombstones().exists(tr, id);

	// If we are trying to create a tenant older than the oldest tombstones we still maintain, then we fail it
	// with an error.
	Optional<TenantTombstoneCleanupData> tombstoneCleanupData = wait(TenantMetadata::tombstoneCleanupData().get(tr));
	if (tombstoneCleanupData.present() && tombstoneCleanupData.get().tombstonesErasedThrough >= id) {
		CODE_PROBE(true, "Tenant creation permanently failed");
		throw tenant_creation_permanently_failed();
	}

	state bool hasTombstone = wait(tombstoneFuture);

	return hasTombstone;
}

// Creates a tenant. If the tenant already exists, the boolean return parameter will be false
// and the existing entry will be returned. If the tenant cannot be created, then the optional will be empty.
ACTOR template <class Transaction>
Future<std::pair<Optional<TenantMapEntry>, bool>>
createTenantTransaction(Transaction tr, TenantMapEntry tenantEntry, ClusterType clusterType = ClusterType::STANDALONE) {
	ASSERT(clusterType != ClusterType::METACLUSTER_MANAGEMENT);
	ASSERT(tenantEntry.id >= 0);

	if (tenantEntry.tenantName.startsWith("\xff"_sr)) {
		CODE_PROBE(true, "Invalid tenant name");
		throw invalid_tenant_name();
	}
	if (tenantEntry.tenantGroup.present() && tenantEntry.tenantGroup.get().startsWith("\xff"_sr)) {
		CODE_PROBE(true, "Invalid tenant group name");
		throw invalid_tenant_group_name();
	}

	tr->setOption(FDBTransactionOptions::RAW_ACCESS);

	state Future<Optional<TenantMapEntry>> existingEntryFuture = tryGetTenantTransaction(tr, tenantEntry.tenantName);
	state Future<Void> tenantModeCheck = checkTenantMode(tr, clusterType);
	state Future<bool> tombstoneFuture =
	    (clusterType == ClusterType::STANDALONE) ? false : checkTombstone(tr, tenantEntry.id);
	state Future<Optional<TenantGroupEntry>> existingTenantGroupEntryFuture;
	if (tenantEntry.tenantGroup.present()) {
		existingTenantGroupEntryFuture = TenantMetadata::tenantGroupMap().get(tr, tenantEntry.tenantGroup.get());
	}

	wait(tenantModeCheck);
	Optional<TenantMapEntry> existingEntry = wait(existingEntryFuture);
	if (existingEntry.present()) {
		CODE_PROBE(true, "Create tenant already exists");
		return std::make_pair(existingEntry.get(), false);
	}

	state bool hasTombstone = wait(tombstoneFuture);
	if (hasTombstone) {
		CODE_PROBE(hasTombstone, "Tenant creation blocked by tombstone");
		return std::make_pair(Optional<TenantMapEntry>(), false);
	}

	state typename transaction_future_type<Transaction, RangeResult>::type prefixRangeFuture =
	    tr->getRange(prefixRange(tenantEntry.prefix), 1);

	RangeResult contents = wait(safeThreadFutureToFuture(prefixRangeFuture));
	if (!contents.empty()) {
		CODE_PROBE(hasTombstone, "Tenant creation conflict with existing data", probe::decoration::rare);
		throw tenant_prefix_allocator_conflict();
	}

	TenantMetadata::tenantMap().set(tr, tenantEntry.id, tenantEntry);
	TenantMetadata::tenantNameIndex().set(tr, tenantEntry.tenantName, tenantEntry.id);
	TenantMetadata::lastTenantModification().setVersionstamp(tr, Versionstamp(), 0);

	if (tenantEntry.tenantGroup.present()) {
		TenantMetadata::tenantGroupTenantIndex().insert(
		    tr, Tuple::makeTuple(tenantEntry.tenantGroup.get(), tenantEntry.tenantName, tenantEntry.id));

		// Create the tenant group associated with this tenant if it doesn't already exist
		Optional<TenantGroupEntry> existingTenantGroup = wait(existingTenantGroupEntryFuture);
		if (!existingTenantGroup.present()) {
			TenantMetadata::tenantGroupMap().set(tr, tenantEntry.tenantGroup.get(), TenantGroupEntry());
		}
	}

	// This is idempotent because we only add an entry to the tenant map if it isn't already there
	TenantMetadata::tenantCount().atomicOp(tr, 1, MutationRef::AddValue);

	// Read the tenant count after incrementing the counter so that simultaneous attempts to create
	// tenants in the same transaction are properly reflected.
	int64_t tenantCount = wait(TenantMetadata::tenantCount().getD(tr, Snapshot::False, 0));
	if (tenantCount > CLIENT_KNOBS->MAX_TENANTS_PER_CLUSTER) {
		CODE_PROBE(true, "Tenant creation would exceed cluster capacity");
		throw cluster_no_capacity();
	}

	return std::make_pair(tenantEntry, true);
}

ACTOR template <class Transaction>
Future<int64_t> getNextTenantId(Transaction tr) {
	state Optional<int64_t> lastId = wait(TenantMetadata::lastTenantId().get(tr));
	if (!lastId.present()) {
		// If the last tenant id is not present fetch the tenantIdPrefix (if any) and initalize the lastId
		int64_t tenantIdPrefix = wait(TenantMetadata::tenantIdPrefix().getD(tr, Snapshot::False, 0));
		// Shift by 6 bytes to make the prefix the first two bytes of the tenant id
		lastId = tenantIdPrefix << 48;
	}

	int64_t delta = 1;
	if (BUGGIFY) {
		delta += deterministicRandom()->randomSkewedUInt32(1, 1e9);
	}

	return TenantAPI::computeNextTenantId(lastId.get(), delta);
}

ACTOR template <class DB>
Future<Optional<TenantMapEntry>> createTenant(Reference<DB> db,
                                              TenantName name,
                                              TenantMapEntry tenantEntry = TenantMapEntry(),
                                              ClusterType clusterType = ClusterType::STANDALONE) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	state bool checkExistence = clusterType != ClusterType::METACLUSTER_DATA;
	state bool generateTenantId = tenantEntry.id < 0;

	CODE_PROBE(generateTenantId, "Create tenant with generated ID");

	ASSERT(clusterType == ClusterType::STANDALONE || !generateTenantId);

	tenantEntry.tenantName = name;

	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);

			state Future<int64_t> tenantIdFuture;
			if (generateTenantId) {
				tenantIdFuture = getNextTenantId(tr);
			}

			if (checkExistence) {
				Optional<int64_t> existingId = wait(TenantMetadata::tenantNameIndex().get(tr, name));
				if (existingId.present()) {
					throw tenant_already_exists();
				}

				checkExistence = false;
			}

			if (generateTenantId) {
				int64_t tenantId = wait(tenantIdFuture);
				tenantEntry.setId(tenantId);
				TenantMetadata::lastTenantId().set(tr, tenantId);
			}

			state std::pair<Optional<TenantMapEntry>, bool> newTenant =
			    wait(createTenantTransaction(tr, tenantEntry, clusterType));

			if (newTenant.second) {
				ASSERT(newTenant.first.present());
				wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));

				TraceEvent("CreatedTenant")
				    .detail("Tenant", name)
				    .detail("TenantId", newTenant.first.get().id)
				    .detail("Prefix", newTenant.first.get().prefix)
				    .detail("TenantGroup", tenantEntry.tenantGroup)
				    .detail("Version", tr->getCommittedVersion());
			}

			return newTenant.first;
		} catch (Error& e) {
			CODE_PROBE(e.code() == error_code_commit_unknown_result, "Create tenant maybe committed");
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

ACTOR template <class Transaction>
Future<Void> markTenantTombstones(Transaction tr, int64_t tenantId) {
	// In data clusters, we store a tombstone
	state Future<KeyBackedRangeResult<int64_t>> latestTombstoneFuture =
	    TenantMetadata::tenantTombstones().getRange(tr, {}, {}, 1, Snapshot::False, Reverse::True);
	state Future<int64_t> tenantIdPrefixFuture = TenantMetadata::tenantIdPrefix().getD(tr, Snapshot::False, 0);
	state Optional<TenantTombstoneCleanupData> cleanupData = wait(TenantMetadata::tombstoneCleanupData().get(tr));
	state Version transactionReadVersion = wait(safeThreadFutureToFuture(tr->getReadVersion()));

	// If the tenant being deleted has a different tenant ID prefix than the current cluster, then it won't conflict
	// with any tenant creations. In that case, we do not need to create a tombstone.
	int64_t tenantIdPrefix = wait(tenantIdPrefixFuture);
	if (tenantIdPrefix != TenantAPI::getTenantIdPrefix(tenantId)) {
		CODE_PROBE(true, "Skipping tenant tombstone for tenant with different prefix");
		return Void();
	}

	// If it has been long enough since we last cleaned up the tenant tombstones, we do that first
	if (!cleanupData.present() || cleanupData.get().nextTombstoneEraseVersion <= transactionReadVersion) {
		state int64_t deleteThroughId = cleanupData.present() ? cleanupData.get().nextTombstoneEraseId : -1;
		// Delete all tombstones up through the one currently marked in the cleanup data
		if (deleteThroughId >= 0) {
			CODE_PROBE(true, "Deleting tenant tombstones");
			TenantMetadata::tenantTombstones().erase(tr, 0, deleteThroughId + 1);
		}

		KeyBackedRangeResult<int64_t> latestTombstone = wait(latestTombstoneFuture);
		int64_t nextDeleteThroughId = std::max(deleteThroughId, tenantId);
		if (!latestTombstone.results.empty()) {
			nextDeleteThroughId = std::max(nextDeleteThroughId, latestTombstone.results[0]);
		}

		// The next cleanup will happen at or after TENANT_TOMBSTONE_CLEANUP_INTERVAL seconds have elapsed and
		// will clean up tombstones through the most recently allocated ID.
		TenantTombstoneCleanupData updatedCleanupData;
		updatedCleanupData.tombstonesErasedThrough = deleteThroughId;
		updatedCleanupData.nextTombstoneEraseId = nextDeleteThroughId;
		updatedCleanupData.nextTombstoneEraseVersion =
		    transactionReadVersion +
		    CLIENT_KNOBS->TENANT_TOMBSTONE_CLEANUP_INTERVAL * CLIENT_KNOBS->VERSIONS_PER_SECOND;

		TenantMetadata::tombstoneCleanupData().set(tr, updatedCleanupData);

		// If the tenant being deleted is within the tombstone window, record the tombstone
		if (tenantId > updatedCleanupData.tombstonesErasedThrough) {
			TenantMetadata::tenantTombstones().insert(tr, tenantId);
		}
	} else if (tenantId > cleanupData.get().tombstonesErasedThrough) {
		// If the tenant being deleted is within the tombstone window, record the tombstone
		TenantMetadata::tenantTombstones().insert(tr, tenantId);
	}
	return Void();
}

// Deletes a tenant with the given ID. If no matching tenant is found, this function returns without deleting anything.
// This behavior allows the function to be used idempotently: if the transaction is retried after having succeeded, it
// will see that the tenant is absent and do nothing.
ACTOR template <class Transaction>
Future<Void> deleteTenantTransaction(Transaction tr,
                                     int64_t tenantId,
                                     ClusterType clusterType = ClusterType::STANDALONE) {
	ASSERT(tenantId != TenantInfo::INVALID_TENANT);
	ASSERT(clusterType != ClusterType::METACLUSTER_MANAGEMENT);

	tr->setOption(FDBTransactionOptions::RAW_ACCESS);

	state Future<Optional<TenantMapEntry>> tenantEntryFuture = tryGetTenantTransaction(tr, tenantId);
	wait(checkTenantMode(tr, clusterType));

	state Optional<TenantMapEntry> tenantEntry = wait(tenantEntryFuture);
	if (tenantEntry.present()) {
		state typename transaction_future_type<Transaction, RangeResult>::type prefixRangeFuture =
		    tr->getRange(prefixRange(tenantEntry.get().prefix), 1);

		RangeResult contents = wait(safeThreadFutureToFuture(prefixRangeFuture));
		if (!contents.empty()) {
			CODE_PROBE(true, "Attempt deletion of non-empty tenant");
			throw tenant_not_empty();
		}

		// This is idempotent because we only erase an entry from the tenant map if it is present
		TenantMetadata::tenantMap().erase(tr, tenantId);
		TenantMetadata::tenantNameIndex().erase(tr, tenantEntry.get().tenantName);
		TenantMetadata::tenantCount().atomicOp(tr, -1, MutationRef::AddValue);
		TenantMetadata::lastTenantModification().setVersionstamp(tr, Versionstamp(), 0);

		if (tenantEntry.get().tenantGroup.present()) {
			TenantMetadata::tenantGroupTenantIndex().erase(
			    tr, Tuple::makeTuple(tenantEntry.get().tenantGroup.get(), tenantEntry.get().tenantName, tenantId));
			KeyBackedSet<Tuple>::RangeResultType tenantsInGroup =
			    wait(TenantMetadata::tenantGroupTenantIndex().getRange(
			        tr,
			        Tuple::makeTuple(tenantEntry.get().tenantGroup.get()),
			        Tuple::makeTuple(keyAfter(tenantEntry.get().tenantGroup.get())),
			        2));
			if (tenantsInGroup.results.empty() ||
			    (tenantsInGroup.results.size() == 1 && tenantsInGroup.results[0].getInt(2) == tenantId)) {
				CODE_PROBE(true, "Deleting tenant results in empty group");
				TenantMetadata::tenantGroupMap().erase(tr, tenantEntry.get().tenantGroup.get());
			}
		}
	} else {
		CODE_PROBE(true, "Delete non-existent tenant");
	}

	if (clusterType == ClusterType::METACLUSTER_DATA) {
		wait(markTenantTombstones(tr, tenantId));
	}

	return Void();
}

// Deletes the tenant with the given name. If tenantId is specified, the tenant being deleted must also have the same
// ID.
ACTOR template <class DB>
Future<Void> deleteTenant(Reference<DB> db,
                          TenantName name,
                          Optional<int64_t> tenantId = Optional<int64_t>(),
                          ClusterType clusterType = ClusterType::STANDALONE) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	state bool checkExistence = clusterType == ClusterType::STANDALONE;
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);

			if (checkExistence) {
				Optional<int64_t> actualId = wait(TenantMetadata::tenantNameIndex().get(tr, name));
				if (!actualId.present() || (tenantId.present() && tenantId != actualId)) {
					CODE_PROBE(!actualId.present(), "Delete non-existing tenant");
					CODE_PROBE(actualId.present(), "Delete tenant with incorrect ID", probe::decoration::rare);
					throw tenant_not_found();
				}

				tenantId = actualId;
				checkExistence = false;
			}

			wait(deleteTenantTransaction(tr, tenantId.get(), clusterType));
			wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));

			TraceEvent("DeletedTenant")
			    .detail("Tenant", name)
			    .detail("TenantId", tenantId)
			    .detail("Version", tr->getCommittedVersion());
			return Void();
		} catch (Error& e) {
			CODE_PROBE(e.code() == error_code_commit_unknown_result, "Delete tenant maybe committed");
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

// This should only be called from a transaction that has already confirmed that the tenant entry
// is present. The tenantEntry should start with the existing entry and modify only those fields that need
// to be changed. This must only be called on a non-management cluster.
ACTOR template <class Transaction>
Future<Void> configureTenantTransaction(Transaction tr,
                                        TenantMapEntry originalEntry,
                                        TenantMapEntry updatedTenantEntry) {
	ASSERT(updatedTenantEntry.id == originalEntry.id);

	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	TenantMetadata::tenantMap().set(tr, updatedTenantEntry.id, updatedTenantEntry);
	TenantMetadata::lastTenantModification().setVersionstamp(tr, Versionstamp(), 0);

	// If the tenant group was changed, we need to update the tenant group metadata structures
	if (originalEntry.tenantGroup != updatedTenantEntry.tenantGroup) {
		if (updatedTenantEntry.tenantGroup.present() && updatedTenantEntry.tenantGroup.get().startsWith("\xff"_sr)) {
			CODE_PROBE(true, "Configure with invalid group name");
			throw invalid_tenant_group_name();
		}
		if (originalEntry.tenantGroup.present()) {
			CODE_PROBE(true, "Change tenant group of tenant already in group");
			// Remove this tenant from the original tenant group index
			TenantMetadata::tenantGroupTenantIndex().erase(
			    tr, Tuple::makeTuple(originalEntry.tenantGroup.get(), originalEntry.tenantName, updatedTenantEntry.id));

			// Check if the original tenant group is now empty. If so, remove the tenant group.
			KeyBackedSet<Tuple>::RangeResultType tenants = wait(TenantMetadata::tenantGroupTenantIndex().getRange(
			    tr,
			    Tuple::makeTuple(originalEntry.tenantGroup.get()),
			    Tuple::makeTuple(keyAfter(originalEntry.tenantGroup.get())),
			    2));

			if (tenants.results.empty() ||
			    (tenants.results.size() == 1 && tenants.results[0].getInt(2) == updatedTenantEntry.id)) {
				CODE_PROBE(true, "Changing tenant group results in empty group");
				TenantMetadata::tenantGroupMap().erase(tr, originalEntry.tenantGroup.get());
			}
		}
		if (updatedTenantEntry.tenantGroup.present()) {
			// If this is creating a new tenant group, add it to the tenant group map
			Optional<TenantGroupEntry> entry =
			    wait(TenantMetadata::tenantGroupMap().get(tr, updatedTenantEntry.tenantGroup.get()));
			if (!entry.present()) {
				CODE_PROBE(true, "Change tenant group to a new group");
				TenantMetadata::tenantGroupMap().set(tr, updatedTenantEntry.tenantGroup.get(), TenantGroupEntry());
			} else {
				CODE_PROBE(true, "Change tenant group to an existing group");
			}

			// Insert this tenant in the tenant group index
			TenantMetadata::tenantGroupTenantIndex().insert(tr,
			                                                Tuple::makeTuple(updatedTenantEntry.tenantGroup.get(),
			                                                                 updatedTenantEntry.tenantName,
			                                                                 updatedTenantEntry.id));
		}
	}

	ASSERT_EQ(updatedTenantEntry.tenantLockId.present(),
	          updatedTenantEntry.tenantLockState != TenantLockState::UNLOCKED);

	return Void();
}

template <class TenantMapEntryT>
bool checkLockState(TenantMapEntryT entry, TenantLockState desiredLockState, UID lockId) {
	if (entry.tenantLockId == lockId && entry.tenantLockState == desiredLockState) {
		CODE_PROBE(true, "Attempting lock change to same state");
		return true;
	}

	if (entry.tenantLockId.present() && entry.tenantLockId.get() != lockId) {
		CODE_PROBE(true, "Attempting invalid lock change");
		throw tenant_locked();
	}

	return false;
}

ACTOR template <class Transaction>
Future<Void> changeLockState(Transaction tr, int64_t tenant, TenantLockState desiredLockState, UID lockId) {
	state Future<Void> tenantModeCheck = TenantAPI::checkTenantMode(tr, ClusterType::STANDALONE);
	state TenantMapEntry entry = wait(TenantAPI::getTenantTransaction(tr, tenant));

	wait(tenantModeCheck);

	if (!checkLockState(entry, desiredLockState, lockId)) {
		TenantMapEntry newState = entry;
		newState.tenantLockState = desiredLockState;
		newState.tenantLockId = (desiredLockState == TenantLockState::UNLOCKED) ? Optional<UID>() : lockId;
		wait(configureTenantTransaction(tr, entry, newState));
	}

	return Void();
}

template <class Transaction>
Future<std::vector<std::pair<TenantName, int64_t>>> listTenantsTransaction(Transaction tr,
                                                                           TenantName begin,
                                                                           TenantName end,
                                                                           int limit) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	auto future = TenantMetadata::tenantNameIndex().getRange(tr, begin, end, limit);
	return fmap([](auto f) -> std::vector<std::pair<TenantName, int64_t>> { return f.results; }, future);
}

template <class DB>
Future<std::vector<std::pair<TenantName, int64_t>>> listTenants(Reference<DB> db,
                                                                TenantName begin,
                                                                TenantName end,
                                                                int limit) {
	return runTransaction(db, [=](Reference<typename DB::TransactionT> tr) {
		tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
		tr->setOption(FDBTransactionOptions::LOCK_AWARE);
		return listTenantsTransaction(tr, begin, end, limit);
	});
}

ACTOR template <class Transaction>
Future<std::vector<std::pair<TenantName, int64_t>>> listTenantGroupTenantsTransaction(Transaction tr,
                                                                                      TenantGroupName tenantGroup,
                                                                                      TenantName begin,
                                                                                      TenantName end,
                                                                                      int limit) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	KeyBackedSet<Tuple>::RangeResultType result = wait(TenantMetadata::tenantGroupTenantIndex().getRange(
	    tr, Tuple::makeTuple(tenantGroup, begin), Tuple::makeTuple(tenantGroup, end), limit));
	std::vector<std::pair<TenantName, int64_t>> returnResult;
	if (!result.results.size()) {
		return returnResult;
	}
	for (auto const& tupleEntry : result.results) {
		returnResult.push_back(std::make_pair(tupleEntry.getString(1), tupleEntry.getInt(2)));
	}
	return returnResult;
}

template <class DB>
Future<std::vector<std::pair<TenantName, int64_t>>> listTenantGroupTenants(Reference<DB> db,
                                                                           TenantGroupName tenantGroup,
                                                                           TenantName begin,
                                                                           TenantName end,
                                                                           int limit) {
	return runTransaction(db, [=](Reference<typename DB::TransactionT> tr) {
		tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
		tr->setOption(FDBTransactionOptions::LOCK_AWARE);
		return listTenantGroupTenantsTransaction(tr, tenantGroup, begin, end, limit);
	});
}

ACTOR template <class Transaction>
Future<std::vector<std::pair<TenantName, TenantMapEntry>>> listTenantMetadataTransaction(Transaction tr,
                                                                                         TenantName begin,
                                                                                         TenantName end,
                                                                                         int limit) {
	std::vector<std::pair<TenantName, int64_t>> matchingTenants = wait(listTenantsTransaction(tr, begin, end, limit));

	state std::vector<Future<TenantMapEntry>> tenantEntryFutures;
	for (auto const& [name, id] : matchingTenants) {
		tenantEntryFutures.push_back(getTenantTransaction(tr, id));
	}

	wait(waitForAll(tenantEntryFutures));

	std::vector<std::pair<TenantName, TenantMapEntry>> results;
	for (auto const& f : tenantEntryFutures) {
		results.emplace_back(f.get().tenantName, f.get());
	}

	return results;
}

template <class DB>
Future<std::vector<std::pair<TenantName, TenantMapEntry>>> listTenantMetadata(Reference<DB> db,
                                                                              TenantName begin,
                                                                              TenantName end,
                                                                              int limit) {
	return runTransaction(db, [=](Reference<typename DB::TransactionT> tr) {
		tr->setOption(FDBTransactionOptions::LOCK_AWARE);
		tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
		return listTenantMetadataTransaction(tr, begin, end, limit);
	});
}

ACTOR template <class Transaction>
Future<Void> renameTenantTransaction(Transaction tr,
                                     TenantName oldName,
                                     TenantName newName,
                                     Optional<int64_t> tenantId = Optional<int64_t>(),
                                     ClusterType clusterType = ClusterType::STANDALONE,
                                     Optional<int64_t> configureSequenceNum = Optional<int64_t>()) {
	ASSERT(clusterType == ClusterType::STANDALONE || (tenantId.present() && configureSequenceNum.present()));
	ASSERT(clusterType != ClusterType::METACLUSTER_MANAGEMENT);

	tr->setOption(FDBTransactionOptions::RAW_ACCESS);

	state Future<Void> tenantModeCheck = checkTenantMode(tr, clusterType);
	state Future<Optional<int64_t>> oldNameIdFuture =
	    tenantId.present() ? Future<Optional<int64_t>>() : TenantMetadata::tenantNameIndex().get(tr, oldName);
	state Future<Optional<int64_t>> newNameIdFuture = TenantMetadata::tenantNameIndex().get(tr, newName);

	wait(tenantModeCheck);

	if (!tenantId.present()) {
		wait(store(tenantId, oldNameIdFuture));
		if (!tenantId.present()) {
			CODE_PROBE(true, "Tenant rename transaction tenant not found");
			throw tenant_not_found();
		}
	}

	state TenantMapEntry entry = wait(getTenantTransaction(tr, tenantId.get()));
	Optional<int64_t> newNameId = wait(newNameIdFuture);
	if (entry.tenantName != oldName) {
		CODE_PROBE(true, "Tenant rename transaction ID/name mismatch");
		throw tenant_not_found();
	}
	if (newNameId.present()) {
		CODE_PROBE(true, "Tenant rename transaction new name already exists");
		throw tenant_already_exists();
	}

	if (configureSequenceNum.present()) {
		if (entry.configurationSequenceNum > configureSequenceNum.get()) {
			CODE_PROBE(true, "Tenant rename transaction already applied", probe::decoration::rare);
			return Void();
		}
		entry.configurationSequenceNum = configureSequenceNum.get();
	}

	entry.tenantName = newName;

	TenantMetadata::tenantMap().set(tr, tenantId.get(), entry);
	TenantMetadata::tenantNameIndex().set(tr, newName, tenantId.get());
	TenantMetadata::tenantNameIndex().erase(tr, oldName);

	if (entry.tenantGroup.present()) {
		CODE_PROBE(true, "Tenant rename transaction inside group");
		TenantMetadata::tenantGroupTenantIndex().erase(
		    tr, Tuple::makeTuple(entry.tenantGroup.get(), oldName, tenantId.get()));
		TenantMetadata::tenantGroupTenantIndex().insert(
		    tr, Tuple::makeTuple(entry.tenantGroup.get(), newName, tenantId.get()));
	}

	TenantMetadata::lastTenantModification().setVersionstamp(tr, Versionstamp(), 0);

	if (clusterType == ClusterType::METACLUSTER_DATA) {
		wait(markTenantTombstones(tr, tenantId.get()));
	}

	return Void();
}

ACTOR template <class DB>
Future<Void> renameTenant(Reference<DB> db,
                          TenantName oldName,
                          TenantName newName,
                          Optional<int64_t> tenantId = Optional<int64_t>(),
                          ClusterType clusterType = ClusterType::STANDALONE) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();
	ASSERT(clusterType == ClusterType::STANDALONE || tenantId.present());

	state bool firstTry = true;
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			if (!tenantId.present()) {
				wait(store(tenantId, TenantMetadata::tenantNameIndex().get(tr, oldName)));
				if (!tenantId.present()) {
					CODE_PROBE(true, "Tenant rename tenant not found");
					throw tenant_not_found();
				}
			}

			state Future<Optional<int64_t>> newNameIdFuture = TenantMetadata::tenantNameIndex().get(tr, newName);
			state TenantMapEntry entry = wait(getTenantTransaction(tr, tenantId.get()));
			Optional<int64_t> newNameId = wait(newNameIdFuture);

			if (!firstTry && entry.tenantName == newName) {
				// On a retry, the rename may have already occurred
				CODE_PROBE(true, "Tenant rename retried and already succeeded");
				return Void();
			} else if (entry.tenantName != oldName) {
				CODE_PROBE(true, "Tenant rename ID/name mismatch");
				throw tenant_not_found();
			} else if (newNameId.present() && newNameId.get() != tenantId.get()) {
				CODE_PROBE(true, "Tenant rename new name already exists");
				throw tenant_already_exists();
			}

			firstTry = false;

			wait(renameTenantTransaction(tr, oldName, newName, tenantId, clusterType));
			wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));

			TraceEvent("TenantRenamed")
			    .detail("OldName", oldName)
			    .detail("NewName", newName)
			    .detail("TenantId", tenantId.get());
			return Void();
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

template <class Transaction>
Future<Optional<TenantGroupEntry>> tryGetTenantGroupTransaction(Transaction tr, TenantGroupName name) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);
	return TenantMetadata::tenantGroupMap().get(tr, name);
}

ACTOR template <class DB>
Future<Optional<TenantGroupEntry>> tryGetTenantGroup(Reference<DB> db, TenantGroupName name) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			Optional<TenantGroupEntry> entry = wait(tryGetTenantGroupTransaction(tr, name));
			return entry;
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

ACTOR template <class Transaction>
Future<std::vector<std::pair<TenantGroupName, TenantGroupEntry>>> listTenantGroupsTransaction(Transaction tr,
                                                                                              TenantGroupName begin,
                                                                                              TenantGroupName end,
                                                                                              int limit) {
	tr->setOption(FDBTransactionOptions::RAW_ACCESS);

	KeyBackedRangeResult<std::pair<TenantGroupName, TenantGroupEntry>> results =
	    wait(TenantMetadata::tenantGroupMap().getRange(tr, begin, end, limit));

	return results.results;
}

ACTOR template <class DB>
Future<std::vector<std::pair<TenantGroupName, TenantGroupEntry>>> listTenantGroups(Reference<DB> db,
                                                                                   TenantGroupName begin,
                                                                                   TenantGroupName end,
                                                                                   int limit) {
	state Reference<typename DB::TransactionT> tr = db->createTransaction();

	loop {
		try {
			tr->setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			std::vector<std::pair<TenantGroupName, TenantGroupEntry>> tenantGroups =
			    wait(listTenantGroupsTransaction(tr, begin, end, limit));
			return tenantGroups;
		} catch (Error& e) {
			wait(safeThreadFutureToFuture(tr->onError(e)));
		}
	}
}

} // namespace TenantAPI

#include "flow/unactorcompiler.h"
#endif
