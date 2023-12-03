/*
 * TenantManagementConcurrencyWorkload.actor.cpp
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

#include <cstdint>
#include <limits>
#include "fdbclient/ClusterConnectionMemoryRecord.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/GenericManagementAPI.actor.h"
#include "fdbclient/MultiVersionTransaction.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/Tenant.h"
#include "fdbclient/TenantManagement.actor.h"
#include "fdbclient/ThreadSafeTransaction.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/Knobs.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/flow.h"

#include "metacluster/Metacluster.h"
#include "metacluster/MetaclusterConsistency.actor.h"
#include "metacluster/TenantConsistency.actor.h"

#include "flow/actorcompiler.h" // This must be the last #include.

struct TenantManagementConcurrencyWorkload : TestWorkload {
	static constexpr auto NAME = "TenantManagementConcurrency";

	const TenantName tenantNamePrefix = "tenant_management_concurrency_workload_"_sr;
	const Key testParametersKey = nonMetadataSystemKeys.begin.withSuffix("/tenant_test/test_parameters"_sr);

	int maxTenants;
	int maxTenantGroups;
	double testDuration;
	bool useMetacluster;
	bool createMetacluster;
	bool allowTenantLimitChanges;

	Reference<IDatabase> managementDb;
	Database standaloneDb;

	TenantManagementConcurrencyWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		maxTenants = std::min<int>(1e8 - 1, getOption(options, "maxTenants"_sr, 100));
		maxTenantGroups = std::min<int>(2 * maxTenants, getOption(options, "maxTenantGroups"_sr, 20));
		testDuration = getOption(options, "testDuration"_sr, 120.0);
		createMetacluster = getOption(options, "createMetacluster"_sr, true);
		allowTenantLimitChanges = getOption(options, "allowTenantLimitChanges"_sr, true);

		if (hasOption(options, "useMetacluster"_sr)) {
			useMetacluster = getOption(options, "useMetacluster"_sr, false);
		} else if (clientId == 0) {
			useMetacluster = deterministicRandom()->coinflip();
		} else {
			// Other clients read the metacluster state from the database
			useMetacluster = false;
		}
	}

	void disableFailureInjectionWorkloads(std::set<std::string>& out) const override { out.insert("Attrition"); }

	struct TestParameters {
		constexpr static FileIdentifier file_identifier = 14350843;

		bool useMetacluster = false;

		TestParameters() {}
		TestParameters(bool useMetacluster) : useMetacluster(useMetacluster) {}

		template <class Ar>
		void serialize(Ar& ar) {
			serializer(ar, useMetacluster);
		}

		Value encode() const { return ObjectWriter::toValue(*this, Unversioned()); }
		static TestParameters decode(ValueRef const& value) {
			return ObjectReader::fromStringRef<TestParameters>(value, Unversioned());
		}
	};

	Future<Void> setup(Database const& cx) override {
		if (allowTenantLimitChanges && clientId == 0 && g_network->isSimulated() && BUGGIFY) {
			IKnobCollection::getMutableGlobalKnobCollection().setKnob(
			    "max_tenants_per_cluster", KnobValueRef::create(int{ deterministicRandom()->randomInt(20, 100) }));
		}

		return _setup(cx, this);
	}
	ACTOR static Future<Void> _setup(Database cx, TenantManagementConcurrencyWorkload* self) {
		state Transaction tr(cx);
		if (self->clientId == 0) {
			// Send test parameters to the other clients
			loop {
				try {
					tr.setOption(FDBTransactionOptions::RAW_ACCESS);
					tr.set(self->testParametersKey, TestParameters(self->useMetacluster).encode());
					wait(tr.commit());
					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
		} else {
			// Read the tenant subspace chosen and saved by client 0
			loop {
				try {
					tr.setOption(FDBTransactionOptions::RAW_ACCESS);
					Optional<Value> val = wait(tr.get(self->testParametersKey));
					if (val.present()) {
						TestParameters params = TestParameters::decode(val.get());
						self->useMetacluster = params.useMetacluster;
						break;
					}

					wait(delay(1.0));
					tr.reset();
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
		}

		if (self->useMetacluster) {
			metacluster::util::SkipMetaclusterCreation skipMetaclusterCreation(!self->createMetacluster ||
			                                                                   self->clientId != 0);

			Optional<metacluster::DataClusterEntry> entry;
			if (!skipMetaclusterCreation) {
				entry = metacluster::DataClusterEntry();
				entry.get().capacity.numTenantGroups = 1e9;
			}

			metacluster::util::SimulatedMetacluster simMetacluster =
			    wait(metacluster::util::createSimulatedMetacluster(cx, {}, entry, skipMetaclusterCreation));

			self->managementDb = simMetacluster.managementDb;
			ASSERT(!simMetacluster.dataDbs.empty());
		} else {
			self->standaloneDb = cx;
		}

		return Void();
	}

	TenantName chooseTenantName() {
		TenantName tenant(
		    format("%s%08d", tenantNamePrefix.toString().c_str(), deterministicRandom()->randomInt(0, maxTenants)));

		return tenant;
	}

	Optional<TenantGroupName> chooseTenantGroup() {
		Optional<TenantGroupName> tenantGroup;
		if (deterministicRandom()->coinflip()) {
			tenantGroup =
			    TenantGroupNameRef(format("tenantgroup%08d", deterministicRandom()->randomInt(0, maxTenantGroups)));
		}

		return tenantGroup;
	}

	ACTOR static Future<Void> createTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName tenant = self->chooseTenantName();
		state metacluster::MetaclusterTenantMapEntry entry;

		state UID debugId = deterministicRandom()->randomUniqueID();

		entry.tenantName = tenant;
		entry.tenantGroup = self->chooseTenantGroup();

		try {
			loop {
				TraceEvent(SevDebug, "TenantManagementConcurrencyCreatingTenant", debugId)
				    .detail("TenantName", entry.tenantName)
				    .detail("TenantGroup", entry.tenantGroup);
				Future<Void> createFuture =
				    self->useMetacluster ? metacluster::createTenant(self->managementDb,
				                                                     entry,
				                                                     metacluster::AssignClusterAutomatically::True,
				                                                     metacluster::IgnoreCapacityLimit::False)
				                         : success(TenantAPI::createTenant(
				                               self->standaloneDb.getReference(), tenant, entry.toTenantMapEntry()));
				Optional<Void> result = wait(timeout(createFuture, 30));
				if (result.present()) {
					TraceEvent(SevDebug, "TenantManagementConcurrencyCreatedTenant", debugId)
					    .detail("TenantName", entry.tenantName)
					    .detail("TenantGroup", entry.tenantGroup);
					break;
				}

				CODE_PROBE(true, "Tenant creation timed out");
			}

			return Void();
		} catch (Error& e) {
			TraceEvent(SevDebug, "TenantManagementConcurrencyCreateTenantError", debugId)
			    .error(e)
			    .detail("TenantName", entry.tenantName)
			    .detail("TenantGroup", entry.tenantGroup);
			if (e.code() == error_code_metacluster_no_capacity || e.code() == error_code_cluster_removed ||
			    e.code() == error_code_cluster_restoring) {
				ASSERT(self->useMetacluster && !self->createMetacluster);
			} else if (e.code() == error_code_tenant_removed) {
				ASSERT(self->useMetacluster);
			} else if (e.code() != error_code_tenant_already_exists && e.code() != error_code_cluster_no_capacity) {
				TraceEvent(SevError, "TenantManagementConcurrencyCreateTenantFailure", debugId)
				    .error(e)
				    .detail("TenantName", entry.tenantName)
				    .detail("TenantGroup", entry.tenantGroup);
				ASSERT(false);
			}

			return Void();
		}
	}

	ACTOR static Future<Void> deleteTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName tenant = self->chooseTenantName();
		state UID debugId = deterministicRandom()->randomUniqueID();

		try {
			loop {
				TraceEvent(SevDebug, "TenantManagementConcurrencyDeletingTenant", debugId).detail("TenantName", tenant);
				Future<Void> deleteFuture = self->useMetacluster
				                                ? metacluster::deleteTenant(self->managementDb, tenant)
				                                : TenantAPI::deleteTenant(self->standaloneDb.getReference(), tenant);
				Optional<Void> result = wait(timeout(deleteFuture, 30));

				if (result.present()) {
					TraceEvent(SevDebug, "TenantManagementConcurrencyDeletedTenant", debugId)
					    .detail("TenantName", tenant);
					break;
				}

				CODE_PROBE(true, "Tenant deletion timed out");
			}

			return Void();
		} catch (Error& e) {
			TraceEvent(SevDebug, "TenantManagementConcurrencyDeleteTenantError", debugId)
			    .error(e)
			    .detail("TenantName", tenant);
			if (e.code() == error_code_cluster_removed || e.code() == error_code_cluster_restoring) {
				ASSERT(self->useMetacluster && !self->createMetacluster);
			} else if (e.code() != error_code_tenant_not_found) {
				TraceEvent(SevError, "TenantManagementConcurrencyDeleteTenantFailure", debugId)
				    .error(e)
				    .detail("TenantName", tenant);

				ASSERT(false);
			}
			return Void();
		}
	}

	ACTOR static Future<Void> configureImpl(TenantManagementConcurrencyWorkload* self,
	                                        TenantName tenant,
	                                        std::map<Standalone<StringRef>, Optional<Value>> configParams,
	                                        metacluster::IgnoreCapacityLimit ignoreCapacityLimit) {
		if (self->useMetacluster) {
			wait(metacluster::configureTenant(self->managementDb, tenant, configParams, ignoreCapacityLimit));
		} else {
			state Reference<ReadYourWritesTransaction> tr = self->standaloneDb->createTransaction();
			loop {
				try {
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					TenantMapEntry entry = wait(TenantAPI::getTenantTransaction(tr, tenant));
					TenantMapEntry updatedEntry = entry;
					for (auto param : configParams) {
						updatedEntry.configure(param.first, param.second);
					}
					wait(TenantAPI::configureTenantTransaction(tr, entry, updatedEntry));
					wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));
					break;
				} catch (Error& e) {
					wait(tr->onError(e));
				}
			}
		}

		return Void();
	}

	ACTOR static Future<Void> configureTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName tenant = self->chooseTenantName();
		state std::map<Standalone<StringRef>, Optional<Value>> configParams;
		state Optional<TenantGroupName> tenantGroup = self->chooseTenantGroup();
		state UID debugId = deterministicRandom()->randomUniqueID();
		state metacluster::IgnoreCapacityLimit ignoreCapacityLimit(deterministicRandom()->coinflip());

		configParams["tenant_group"_sr] = tenantGroup;

		try {
			loop {
				TraceEvent(SevDebug, "TenantManagementConcurrencyConfiguringTenant", debugId)
				    .detail("TenantName", tenant)
				    .detail("TenantGroup", tenantGroup);
				Optional<Void> result =
				    wait(timeout(configureImpl(self, tenant, configParams, ignoreCapacityLimit), 30));

				if (result.present()) {
					TraceEvent(SevDebug, "TenantManagementConcurrencyConfiguredTenant", debugId)
					    .detail("TenantName", tenant)
					    .detail("TenantGroup", tenantGroup);
					break;
				}

				CODE_PROBE(true, "Tenant configure timed out");
			}

			return Void();
		} catch (Error& e) {
			TraceEvent(SevDebug, "TenantManagementConcurrencyConfigureTenantError", debugId)
			    .error(e)
			    .detail("TenantName", tenant)
			    .detail("TenantGroup", tenantGroup);
			if (e.code() == error_code_cluster_removed || e.code() == error_code_cluster_restoring) {
				ASSERT(self->useMetacluster && !self->createMetacluster);
			} else if (e.code() == error_code_cluster_no_capacity ||
			           e.code() == error_code_invalid_tenant_configuration) {
				ASSERT(self->useMetacluster && !self->createMetacluster);
			} else if (e.code() != error_code_tenant_not_found && e.code() != error_code_invalid_tenant_state) {
				TraceEvent(SevError, "TenantManagementConcurrencyConfigureTenantFailure", debugId)
				    .error(e)
				    .detail("TenantName", tenant)
				    .detail("TenantGroup", tenantGroup);
				ASSERT(false);
			}
			return Void();
		}
	}

	ACTOR static Future<Void> renameTenant(TenantManagementConcurrencyWorkload* self) {
		state TenantName oldTenant = self->chooseTenantName();
		state TenantName newTenant = self->chooseTenantName();
		state UID debugId = deterministicRandom()->randomUniqueID();

		try {
			loop {
				TraceEvent(SevDebug, "TenantManagementConcurrencyRenamingTenant", debugId)
				    .detail("OldTenantName", oldTenant)
				    .detail("NewTenantName", newTenant);
				Future<Void> renameFuture =
				    self->useMetacluster
				        ? metacluster::renameTenant(self->managementDb, oldTenant, newTenant)
				        : TenantAPI::renameTenant(self->standaloneDb.getReference(), oldTenant, newTenant);
				Optional<Void> result = wait(timeout(renameFuture, 30));

				if (result.present()) {
					TraceEvent(SevDebug, "TenantManagementConcurrencyRenamedTenant", debugId)
					    .detail("OldTenantName", oldTenant)
					    .detail("NewTenantName", newTenant);
					break;
				}

				CODE_PROBE(true, "Tenant rename timed out");
			}

			return Void();
		} catch (Error& e) {
			TraceEvent(SevDebug, "TenantManagementConcurrencyRenameTenantError", debugId)
			    .error(e)
			    .detail("OldTenantName", oldTenant)
			    .detail("NewTenantName", newTenant);
			if (e.code() == error_code_cluster_removed || e.code() == error_code_cluster_restoring) {
				ASSERT(self->useMetacluster && !self->createMetacluster);
			} else if (e.code() == error_code_invalid_tenant_state || e.code() == error_code_tenant_removed ||
			           e.code() == error_code_cluster_no_capacity) {
				ASSERT(self->useMetacluster);
			} else if (e.code() != error_code_tenant_not_found && e.code() != error_code_tenant_already_exists) {
				TraceEvent(SevDebug, "TenantManagementConcurrencyRenameTenantFailure", debugId)
				    .error(e)
				    .detail("OldTenantName", oldTenant)
				    .detail("NewTenantName", newTenant);
				ASSERT(false);
			}
			return Void();
		}
	}

	ACTOR static Future<Void> changeLockStateImpl(TenantManagementConcurrencyWorkload* self,
	                                              TenantName tenant,
	                                              TenantAPI::TenantLockState lockState,
	                                              bool useExistingId) {
		state UID lockId;
		if (self->useMetacluster) {
			metacluster::MetaclusterTenantMapEntry entry = wait(metacluster::getTenant(self->managementDb, tenant));
			if (useExistingId && entry.tenantLockId.present()) {
				lockId = entry.tenantLockId.get();
			} else {
				lockId = deterministicRandom()->randomUniqueID();
			}

			wait(metacluster::changeTenantLockState(self->managementDb, tenant, lockState, lockId));
		} else {
			state Reference<ReadYourWritesTransaction> tr = self->standaloneDb->createTransaction();
			loop {
				try {
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					TenantMapEntry entry = wait(TenantAPI::getTenantTransaction(tr, tenant));
					if (useExistingId && entry.tenantLockId.present()) {
						lockId = entry.tenantLockId.get();
					} else {
						lockId = deterministicRandom()->randomUniqueID();
					}

					wait(TenantAPI::changeLockState(tr, entry.id, lockState, lockId));
					wait(buggifiedCommit(tr, BUGGIFY_WITH_PROB(0.1)));
					break;
				} catch (Error& e) {
					wait(tr->onError(e));
				}
			}
		}

		return Void();
	}

	ACTOR static Future<Void> changeLockState(TenantManagementConcurrencyWorkload* self) {
		state TenantName tenant = self->chooseTenantName();
		state TenantAPI::TenantLockState lockState = (TenantAPI::TenantLockState)deterministicRandom()->randomInt(0, 3);
		state bool useExistingId = deterministicRandom()->coinflip();
		state UID debugId = deterministicRandom()->randomUniqueID();

		try {
			loop {
				TraceEvent(SevDebug, "TenantManagementConcurrencyChangingTenantLockState", debugId)
				    .detail("TenantName", tenant)
				    .detail("TenantLockState", TenantAPI::tenantLockStateToString(lockState))
				    .detail("UseExistingId", useExistingId);

				Optional<Void> result = wait(timeout(changeLockStateImpl(self, tenant, lockState, useExistingId), 30));

				if (result.present()) {
					TraceEvent(SevDebug, "TenantManagementConcurrencyChangedTenantLockState", debugId)
					    .detail("TenantName", tenant)
					    .detail("TenantLockState", TenantAPI::tenantLockStateToString(lockState))
					    .detail("UseExistingId", useExistingId);
					break;
				}

				CODE_PROBE(true, "Tenant change lock state timed out");
			}

			return Void();
		} catch (Error& e) {
			TraceEvent(SevDebug, "TenantManagementConcurrencyChangeLockStateError", debugId)
			    .error(e)
			    .detail("TenantName", tenant)
			    .detail("TenantLockState", TenantAPI::tenantLockStateToString(lockState))
			    .detail("UseExistingId", useExistingId);
			if (e.code() == error_code_cluster_removed || e.code() == error_code_cluster_restoring) {
				ASSERT(self->useMetacluster && !self->createMetacluster);
			} else if (e.code() != error_code_tenant_not_found && e.code() != error_code_tenant_locked &&
			           e.code() != error_code_invalid_tenant_state) {
				TraceEvent(SevError, "TenantManagementConcurrencyChangeLockStateFailure", debugId)
				    .error(e)
				    .detail("TenantName", tenant)
				    .detail("TenantLockState", TenantAPI::tenantLockStateToString(lockState))
				    .detail("UseExistingId", useExistingId);
				ASSERT(false);
			}
			return Void();
		}
	}

	Future<Void> start(Database const& cx) override { return _start(cx, this); }
	ACTOR static Future<Void> _start(Database cx, TenantManagementConcurrencyWorkload* self) {
		state double start = now();

		// Run a random sequence of tenant management operations for the duration of the test
		while (now() < start + self->testDuration) {
			state int operation = deterministicRandom()->randomInt(0, 5);
			if (operation == 0) {
				wait(createTenant(self));
			} else if (operation == 1) {
				wait(deleteTenant(self));
			} else if (operation == 2) {
				wait(configureTenant(self));
			} else if (operation == 3) {
				wait(renameTenant(self));
			} else if (operation == 4) {
				wait(changeLockState(self));
			}
		}

		return Void();
	}

	Future<bool> check(Database const& cx) override { return _check(cx, this); }
	ACTOR static Future<bool> _check(Database cx, TenantManagementConcurrencyWorkload* self) {
		if (self->useMetacluster) {
			// The metacluster consistency check runs the tenant consistency check for each cluster
			state metacluster::util::MetaclusterConsistencyCheck<IDatabase> metaclusterConsistencyCheck(
			    self->managementDb, metacluster::util::AllowPartialMetaclusterOperations::True);
			wait(metaclusterConsistencyCheck.run());
		} else {
			state metacluster::util::TenantConsistencyCheck<DatabaseContext, StandardTenantTypes>
			    tenantConsistencyCheck(self->standaloneDb.getReference(), &TenantMetadata::instance());
			wait(tenantConsistencyCheck.run());
		}

		return true;
	}

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<TenantManagementConcurrencyWorkload> TenantManagementConcurrencyWorkloadFactory;
