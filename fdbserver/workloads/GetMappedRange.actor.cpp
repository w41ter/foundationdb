/*
 * GetMappedRange.actor.cpp
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

#include <algorithm>
#include "fdbclient/MutationLogReader.actor.h"
#include "fdbclient/StatusClient.h"
#include "fdbclient/Tuple.h"
#include "fdbserver/workloads/ApiWorkload.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/Knobs.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/flow.h"
#include "flow/actorcompiler.h" // This must be the last #include.

const Value EMPTY = Tuple().pack();
ValueRef SOMETHING = "SOMETHING"_sr;
const KeyRef prefix = "prefix"_sr;
const KeyRef RECORD = "RECORD"_sr;
const KeyRef INDEX = "INDEX"_sr;

int recordSize;
int indexSize;
struct GetMappedRangeWorkload : ApiWorkload {
	static constexpr auto NAME = "GetMappedRange";
	bool enabled;
	Snapshot snapshot = Snapshot::False;

	//	const bool BAD_MAPPER = deterministicRandom()->random01() < 0.1;
	const bool BAD_MAPPER = false;
	//	const bool SPLIT_RECORDS = deterministicRandom()->random01() < 0.5;
	const bool SPLIT_RECORDS = true;
	const static int SPLIT_SIZE = 3;
	double checkStorageQueueSeconds;
	uint64_t queueMaxLength;

	GetMappedRangeWorkload(WorkloadContext const& wcx) : ApiWorkload(wcx) {
		enabled = !clientId; // only do this on the "first" client
		checkStorageQueueSeconds = getOption(options, "checkStorageQueueSeconds"_sr, 60.0);
		queueMaxLength = getOption(options, "queueMaxLength"_sr, UINT64_C(100));
	}

	// TODO: Currently this workload doesn't play well with MachineAttrition, but it probably should
	void disableFailureInjectionWorkloads(std::set<std::string>& out) const override { out.insert("Attrition"); }

	Future<Void> start(Database const& cx) override {
		// This workload is generated different from typical ApiWorkload. So don't use ApiWorkload::_start.
		if (enabled) {
			return GetMappedRangeWorkload::_start(cx, this);
		}
		return Void();
	}

	ACTOR Future<Void> performSetup(Database cx, GetMappedRangeWorkload* self) {
		std::vector<TransactionType> types;
		types.push_back(NATIVE);
		types.push_back(READ_YOUR_WRITES);

		wait(self->chooseTransactionFactory(cx, types));
		return Void();
	}

	Future<Void> performSetup(Database const& cx) override { return performSetup(cx, this); }

	Future<Void> performTest(Database const& cx, Standalone<VectorRef<KeyValueRef>> const& data) override {
		// Ignore this because we are not using ApiWorkload's default ::start.
		return Future<Void>();
	}

	static Key primaryKey(int i) { return Key(format("primary-key-of-record-%08d", i)); }
	static Key indexKey(int i) { return Key(format("index-key-of-record-%08d", i)); }
	static Value dataOfRecord(int i) { return Key(format("data-of-record-%08d", i)); }
	static Value dataOfRecord(int i, int split) { return Key(format("data-of-record-%08d-split-%08d", i, split)); }

	static Key indexEntryKey(int i) { return Tuple::makeTuple(prefix, INDEX, indexKey(i), primaryKey(i)).pack(); }
	static Key recordKey(int i) { return Tuple::makeTuple(prefix, RECORD, primaryKey(i)).pack(); }
	static Key recordKey(int i, int split) { return Tuple::makeTuple(prefix, RECORD, primaryKey(i), split).pack(); }
	static Value recordValue(int i) { return Tuple::makeTuple(dataOfRecord(i)).pack(); }
	static Value recordValue(int i, int split) { return Tuple::makeTuple(dataOfRecord(i, split)).pack(); }

	ACTOR Future<Void> fillInRecords(Database cx, int n, GetMappedRangeWorkload* self) {
		state Transaction tr(cx);
		loop {
			std::cout << "start fillInRecords n=" << n << std::endl;
			// TODO: When n is large, split into multiple transactions.
			recordSize = 0;
			indexSize = 0;
			try {
				for (int i = 0; i < n; i++) {
					if (self->SPLIT_RECORDS) {
						for (int split = 0; split < SPLIT_SIZE; split++) {
							tr.set(recordKey(i, split), recordValue(i, split));
							if (i == 0) {
								recordSize +=
								    recordKey(i, split).size() + recordValue(i, split).size() + sizeof(KeyValueRef);
							}
						}
					} else {
						tr.set(recordKey(i), recordValue(i));
						if (i == 0) {
							recordSize += recordKey(i).size() + recordValue(i).size() + sizeof(KeyValueRef);
						}
					}
					tr.set(indexEntryKey(i), EMPTY);
					if (i == 0) {
						indexSize += indexEntryKey(i).size() + sizeof(KeyValueRef);
					}
				}
				wait(tr.commit());
				std::cout << "finished fillInRecords with version " << tr.getCommittedVersion() << " recordSize "
				          << recordSize << " indexSize " << indexSize << std::endl;
				break;
			} catch (Error& e) {
				std::cout << "failed fillInRecords, retry" << std::endl;
				wait(tr.onError(e));
			}
		}
		return Void();
	}

	static void showResult(const RangeResult& result) {
		std::cout << "result size: " << result.size() << std::endl;
		for (const KeyValueRef* it = result.begin(); it != result.end(); it++) {
			std::cout << "key=" << it->key.printable() << ", value=" << it->value.printable() << std::endl;
		}
	}

	ACTOR Future<Void> scanRange(Database cx, KeyRangeRef range) {
		std::cout << "start scanRange " << range.toString() << std::endl;
		// TODO: When n is large, split into multiple transactions.
		state Transaction tr(cx);
		loop {
			try {
				RangeResult result = wait(tr.getRange(range, CLIENT_KNOBS->TOO_MANY));
				//			showResult(result);
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
		std::cout << "finished scanRange" << std::endl;
		return Void();
	}

	// Return true if need to retry.
	static bool validateRecord(int expectedId,
	                           const MappedKeyValueRef* it,
	                           GetMappedRangeWorkload* self,
	                           bool allMissing) {
		// std::cout << "validateRecord expectedId " << expectedId << " it->key " << printable(it->key)
		//           << " indexEntryKey(expectedId) " << printable(indexEntryKey(expectedId)) << std::endl;

		ASSERT(it->key == indexEntryKey(expectedId));
		ASSERT(it->value == EMPTY);

		if (self->SPLIT_RECORDS) {
			ASSERT(std::holds_alternative<GetRangeReqAndResultRef>(it->reqAndResult));
			auto& getRange = std::get<GetRangeReqAndResultRef>(it->reqAndResult);
			auto& rangeResult = getRange.result;
			//					std::cout << "rangeResult.size()=" << rangeResult.size() << std::endl;
			// In the future, we may be able to do the continuation more efficiently by combining partial results
			// together and then validate.
			if (rangeResult.more) {
				// Retry if the underlying request is not fully completed.
				return true;
			}
			if (!allMissing) {
				ASSERT(rangeResult.size() == SPLIT_SIZE);
				for (int split = 0; split < SPLIT_SIZE; split++) {
					auto& kv = rangeResult[split];
					//				std::cout << "kv.key=" << printable(kv.key)
					//						   << ", recordKey(id, split)=" << printable(recordKey(id, split)) <<
					// std::endl; std::cout << "kv.value=" << printable(kv.value)
					//						   << ", recordValue(id, split)=" << printable(recordValue(id,split)) <<
					// std::endl;
					ASSERT(kv.key == recordKey(expectedId, split));
					ASSERT(kv.value == recordValue(expectedId, split));
				}
			}

		} else {
			ASSERT(std::holds_alternative<GetValueReqAndResultRef>(it->reqAndResult));
			auto& getValue = std::get<GetValueReqAndResultRef>(it->reqAndResult);
			ASSERT(getValue.key == recordKey(expectedId));
			ASSERT(getValue.result.present());
			ASSERT(getValue.result.get() == recordValue(expectedId));
		}
		return false;
	}

	ACTOR Future<MappedRangeResult> scanMappedRangeWithLimits(Database cx,
	                                                          KeySelector beginSelector,
	                                                          KeySelector endSelector,
	                                                          Key mapper,
	                                                          int limit,
	                                                          int byteLimit,
	                                                          int expectedBeginId,
	                                                          GetMappedRangeWorkload* self,
	                                                          bool allMissing) {

		std::cout << "start scanMappedRangeWithLimits beginSelector:" << beginSelector.toString()
		          << " endSelector:" << endSelector.toString() << " expectedBeginId:" << expectedBeginId
		          << " limit:" << limit << " byteLimit: " << byteLimit << "  recordSize: " << recordSize
		          << " STRICTLY_ENFORCE_BYTE_LIMIT: " << SERVER_KNOBS->STRICTLY_ENFORCE_BYTE_LIMIT << " allMissing "
		          << allMissing << std::endl;
		loop {
			state Reference<TransactionWrapper> tr = self->createTransaction();
			try {
				MappedRangeResult result = wait(tr->getMappedRange(beginSelector,
				                                                   endSelector,
				                                                   mapper,
				                                                   GetRangeLimits(limit, byteLimit),
				                                                   self->snapshot,
				                                                   Reverse::False));
				//			showResult(result);
				if (self->BAD_MAPPER) {
					TraceEvent("GetMappedRangeWorkloadShouldNotReachable").detail("ResultSize", result.size());
				}
				std::cout << "result.size()=" << result.size() << std::endl;
				std::cout << "result.more=" << result.more << std::endl;
				ASSERT(result.size() <= limit);
				int expectedId = expectedBeginId;
				bool needRetry = false;
				int cnt = 0;
				const MappedKeyValueRef* it = result.begin();
				for (; cnt < result.size(); cnt++, it++) {
					if (validateRecord(expectedId, it, self, allMissing)) {
						needRetry = true;
						break;
					}
					expectedId++;
				}
				if (needRetry) {
					continue;
				}
				std::cout << "finished scanMappedRangeWithLimits" << std::endl;
				return result;
			} catch (Error& e) {
				if ((self->BAD_MAPPER && e.code() == error_code_mapper_bad_index) ||
				    (!SERVER_KNOBS->QUICK_GET_VALUE_FALLBACK && e.code() == error_code_quick_get_value_miss) ||
				    (!SERVER_KNOBS->QUICK_GET_KEY_VALUES_FALLBACK &&
				     e.code() == error_code_quick_get_key_values_miss)) {
					TraceEvent("GetMappedRangeWorkloadExpectedErrorDetected").error(e);
					return MappedRangeResult();
				} else if (e.code() == error_code_commit_proxy_memory_limit_exceeded ||
				           e.code() == error_code_operation_cancelled) {
					// requests have overwhelmed commit proxy, rest a bit
					wait(delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY));
					continue;
				} else {
					std::cout << "scan error " << e.what() << "  code is " << e.code() << std::endl;
					wait(tr->onError(e));
				}
				std::cout << "failed scanMappedRangeWithLimits" << std::endl;
			}
		}
	}

	// if sendFirstRequestIndefinitely is true, then this method would send the first request indefinitly
	// it is in order to test the metric
	ACTOR Future<Void> submitSmallRequestIndefinitely(Database cx,
	                                                  int beginId,
	                                                  int endId,
	                                                  Key mapper,
	                                                  GetMappedRangeWorkload* self) {
		Key beginTuple = Tuple().append(prefix).append(INDEX).append(indexKey(beginId)).getDataAsStandalone();
		state KeySelector beginSelector = KeySelector(firstGreaterOrEqual(beginTuple));
		Key endTuple = Tuple().append(prefix).append(INDEX).append(indexKey(endId)).getDataAsStandalone();
		state KeySelector endSelector = KeySelector(firstGreaterOrEqual(endTuple));
		state int limit = 1;
		state int byteLimit = 10000;
		while (true) {
			MappedRangeResult result = wait(self->scanMappedRangeWithLimits(
			    cx, beginSelector, endSelector, mapper, limit, byteLimit, beginId, self, false));
			if (result.empty()) {
				TraceEvent("EmptyResult");
			}
			// to avoid requests make proxy memory overwhelmed
			wait(delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY));
		}
	}

	ACTOR Future<Void> scanMappedRange(Database cx,
	                                   int beginId,
	                                   int endId,
	                                   Key mapper,
	                                   GetMappedRangeWorkload* self,
	                                   bool allMissing = false) {
		Key beginTuple = Tuple::makeTuple(prefix, INDEX, indexKey(beginId)).getDataAsStandalone();
		state KeySelector beginSelector = KeySelector(firstGreaterOrEqual(beginTuple));
		Key endTuple = Tuple::makeTuple(prefix, INDEX, indexKey(endId)).getDataAsStandalone();
		state KeySelector endSelector = KeySelector(firstGreaterOrEqual(endTuple));
		state int limit = 100;
		state int byteLimit = deterministicRandom()->randomInt(1, 9) * 10000;
		state int expectedBeginId = beginId;
		std::cout << "ByteLimit: " << byteLimit << " limit: " << limit
		          << " FRACTION_INDEX_BYTELIMIT_PREFETCH: " << SERVER_KNOBS->FRACTION_INDEX_BYTELIMIT_PREFETCH
		          << " MAX_PARALLEL_QUICK_GET_VALUE: " << SERVER_KNOBS->MAX_PARALLEL_QUICK_GET_VALUE << std::endl;
		while (true) {
			MappedRangeResult result = wait(self->scanMappedRangeWithLimits(
			    cx, beginSelector, endSelector, mapper, limit, byteLimit, expectedBeginId, self, allMissing));
			expectedBeginId += result.size();
			if (result.more) {
				if (result.empty()) {
					// This is usually not expected.
					std::cout << "not result but have more, try again" << std::endl;
				} else {
					int size = allMissing ? indexSize : (indexSize + recordSize);
					int expectedCnt = limit;
					int indexByteLimit = byteLimit * SERVER_KNOBS->FRACTION_INDEX_BYTELIMIT_PREFETCH;
					int indexCountByteLimit = indexByteLimit / indexSize + (indexByteLimit % indexSize != 0);
					int indexCount = std::min(limit, indexCountByteLimit);
					// result set cannot be larger than the number of index fetched
					ASSERT(result.size() <= indexCount);

					expectedCnt = std::min(expectedCnt, indexCount);
					int boundByRecord;
					if (SERVER_KNOBS->STRICTLY_ENFORCE_BYTE_LIMIT) {
						// might have 1 additional entry over the limit
						boundByRecord = byteLimit / size + (byteLimit % size != 0);
					} else {
						// might have 1 additional batch over the limit
						int roundSize = size * SERVER_KNOBS->MAX_PARALLEL_QUICK_GET_VALUE;
						int round = byteLimit / roundSize + (byteLimit % roundSize != 0);
						boundByRecord = round * SERVER_KNOBS->MAX_PARALLEL_QUICK_GET_VALUE;
					}
					expectedCnt = std::min(expectedCnt, boundByRecord);
					ASSERT_LE(result.size(), expectedCnt);
					beginSelector = KeySelector(firstGreaterThan(result.back().key));
				}
			} else {
				// No more, finished.
				break;
			}
		}
		ASSERT(expectedBeginId == endId);

		return Void();
	}

	static void conflictWriteOnRecord(int conflictRecordId,
	                                  Reference<TransactionWrapper>& tr,
	                                  GetMappedRangeWorkload* self) {
		Key writeKey;
		if (deterministicRandom()->random01() < 0.5) {
			// Concurrent write to the primary scanned range
			writeKey = indexEntryKey(conflictRecordId);
		} else {
			// Concurrent write to the underlying scanned ranges/keys
			if (self->SPLIT_RECORDS) {
				// Update one of the splits is sufficient.
				writeKey = recordKey(conflictRecordId, 0);
			} else {
				writeKey = recordKey(conflictRecordId);
			}
		}
		tr->set(writeKey, SOMETHING);
		std::cout << "conflict write to " << printable(writeKey) << std::endl;
	}

	static Future<MappedRangeResult> runGetMappedRange(int beginId,
	                                                   int endId,
	                                                   Reference<TransactionWrapper>& tr,
	                                                   GetMappedRangeWorkload* self) {
		Key mapper = getMapper(self, false);
		Key beginTuple = Tuple::makeTuple(prefix, INDEX, indexKey(beginId)).getDataAsStandalone();
		KeySelector beginSelector = KeySelector(firstGreaterOrEqual(beginTuple));
		Key endTuple = Tuple::makeTuple(prefix, INDEX, indexKey(endId)).getDataAsStandalone();
		KeySelector endSelector = KeySelector(firstGreaterOrEqual(endTuple));
		return tr->getMappedRange(beginSelector,
		                          endSelector,
		                          mapper,
		                          GetRangeLimits(GetRangeLimits::ROW_LIMIT_UNLIMITED),
		                          self->snapshot,
		                          Reverse::False);
	}

	// If another transaction writes to our read set (the scanned ranges) before we commit, the transaction should
	// fail.
	ACTOR Future<Void> testSerializableConflicts(GetMappedRangeWorkload* self) {
		std::cout << "testSerializableConflicts" << std::endl;

		loop {
			state Reference<TransactionWrapper> tr1 = self->createTransaction();
			try {
				MappedRangeResult result = wait(runGetMappedRange(5, 10, tr1, self));

				// Commit another transaction that has conflict writes.
				loop {
					state Reference<TransactionWrapper> tr2 = self->createTransaction();
					try {
						conflictWriteOnRecord(7, tr2, self);
						wait(tr2->commit());
						break;
					} catch (Error& e) {
						std::cout << "tr2 error " << e.what() << std::endl;
						wait(tr2->onError(e));
					}
				}

				// Do some writes so that tr1 is not read-only.
				tr1->set(SOMETHING, SOMETHING);
				wait(tr1->commit());
				UNREACHABLE();
			} catch (Error& e) {
				if (e.code() == error_code_not_committed) {
					std::cout << "tr1 failed because of conflicts (as expected)" << std::endl;
					TraceEvent("GetMappedRangeWorkloadExpectedErrorDetected").error(e);
					return Void();
				} else {
					std::cout << "tr1 error " << e.what() << std::endl;
					wait(tr1->onError(e));
				}
			}
		}
	}

	// checking the max storage queue length is bounded
	ACTOR static Future<Void> reportMetric(GetMappedRangeWorkload* self, Database cx) {
		loop {
			StatusObject result = wait(StatusClient::statusFetcher(cx));
			StatusObjectReader statusObj(result);
			state StatusObjectReader statusObjCluster;
			state StatusObjectReader processesMap;
			state int64_t queryQueueMax = 0;
			state int waitInterval = 2;
			if (!statusObj.get("cluster", statusObjCluster)) {
				TraceEvent("NoCluster");
				wait(delay(waitInterval));
				continue;
			}

			if (!statusObjCluster.get("processes", processesMap)) {
				TraceEvent("NoProcesses");
				wait(delay(waitInterval));
				continue;
			}
			for (auto proc : processesMap.obj()) {
				StatusObjectReader process(proc.second);
				if (process.has("roles")) {
					StatusArray rolesArray = proc.second.get_obj()["roles"].get_array();
					for (StatusObjectReader role : rolesArray) {
						if (role["role"].get_str() == "storage") {
							role.get("query_queue_max", queryQueueMax);
							CODE_PROBE(queryQueueMax > 0, " SS query queue is non-empty");
							TraceEvent(SevDebug, "QueryQueueMax")
							    .detail("Value", queryQueueMax)
							    .detail("MaxLength", self->queueMaxLength);
							ASSERT(queryQueueMax < self->queueMaxLength);
						}
					}
				} else {
					TraceEvent("NoRoles");
				}
			}
			wait(delay(waitInterval));
		}
	}

	// If the same transaction writes to the read set (the scanned ranges) before reading, it should throw read your
	// write exception.
	ACTOR Future<Void> testRYW(GetMappedRangeWorkload* self) {
		std::cout << "testRYW" << std::endl;
		loop {
			state Reference<TransactionWrapper> tr1 = self->createTransaction();
			try {
				// Write something that will be read in getMappedRange.
				conflictWriteOnRecord(7, tr1, self);
				MappedRangeResult result = wait(runGetMappedRange(5, 10, tr1, self));
				UNREACHABLE();
			} catch (Error& e) {
				if (e.code() == error_code_get_mapped_range_reads_your_writes) {
					std::cout << "tr1 failed because of read your writes (as expected)" << std::endl;
					TraceEvent("GetMappedRangeWorkloadExpectedErrorDetected").error(e);
					return Void();
				} else {
					std::cout << "tr1 error " << e.what() << std::endl;
					wait(tr1->onError(e));
				}
			}
		}
	}

	ACTOR static Future<Void> testMetric(Database cx,
	                                     GetMappedRangeWorkload* self,
	                                     int beginId,
	                                     int endId,
	                                     Key mapper,
	                                     int seconds) {
		loop choose {
			when(wait(reportMetric(self, cx))) {
				TraceEvent(SevError, "Error: ReportMetric has ended");
				return Void();
			}
			when(wait(self->submitSmallRequestIndefinitely(cx, 10, 490, mapper, self))) {
				TraceEvent(SevError, "Error: submitSmallRequestIndefinitely has ended");
				return Void();
			}
			when(wait(delay(seconds))) {
				return Void();
			}
		}
	}

	ACTOR Future<Void> _start(Database cx, GetMappedRangeWorkload* self) {
		TraceEvent("GetMappedRangeWorkloadConfig").detail("BadMapper", self->BAD_MAPPER);

		// TODO: Use toml to config
		wait(self->fillInRecords(cx, 500, self));

		if (self->transactionType == NATIVE) {
			self->snapshot = Snapshot::True;
		} else if (self->transactionType == READ_YOUR_WRITES) {
			self->snapshot = Snapshot::False;
			const double rand = deterministicRandom()->random01();
			if (rand < 0.1) {
				wait(self->testSerializableConflicts(self));
				return Void();
			} else if (rand < 0.2) {
				wait(self->testRYW(self));
				return Void();
			} else {
				// Test the happy path where there is no conflicts or RYW
			}
		} else {
			UNREACHABLE();
		}

		std::cout << "Test configuration: transactionType:" << self->transactionType << " snapshot:" << self->snapshot
		          << "bad_mapper:" << self->BAD_MAPPER << std::endl;

		state Key mapper = getMapper(self, false);
		// The scanned range cannot be too large to hit get_mapped_key_values_has_more. We have a unit validating the
		// error is thrown when the range is large.
		state bool originalStrictlyEnforeByteLimit = SERVER_KNOBS->STRICTLY_ENFORCE_BYTE_LIMIT;
		(const_cast<ServerKnobs*> SERVER_KNOBS)->STRICTLY_ENFORCE_BYTE_LIMIT = deterministicRandom()->coinflip();
		wait(self->scanMappedRange(cx, 10, 490, mapper, self));
		wait(testMetric(cx, self, 10, 490, mapper, self->checkStorageQueueSeconds));

		// reset it to default
		(const_cast<ServerKnobs*> SERVER_KNOBS)->STRICTLY_ENFORCE_BYTE_LIMIT = originalStrictlyEnforeByteLimit;
		return Void();
	}

	static Key getMapper(GetMappedRangeWorkload* self, bool mapperForAllMissing) {
		Tuple mapperTuple;
		if (self->BAD_MAPPER) {
			mapperTuple << prefix << RECORD << "{K[xxx]}"_sr;
		} else {
			mapperTuple << prefix << RECORD << (mapperForAllMissing ? "{K[2]}"_sr : "{K[3]}"_sr);
			if (self->SPLIT_RECORDS) {
				mapperTuple << "{...}"_sr;
			}
		}
		Key mapper = mapperTuple.getDataAsStandalone();
		return mapper;
	}

	Future<bool> check(Database const& cx) override { return true; }

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<GetMappedRangeWorkload> GetMappedRangeWorkloadFactory;
