/*
 * KVStoreTest.actor.cpp
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

#include <ctime>
#include <cinttypes>
#include "fdbclient/FDBTypes.h"
#include "fmt/format.h"
#include "fdbserver/ServerDBInfo.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "flow/ActorCollection.h"
#include "flow/actorcompiler.h" // This must be the last #include.

extern IKeyValueStore* makeDummyKeyValueStore();

template <class T>
class TestHistogram {
public:
	TestHistogram(int minSamples = 100) : minSamples(minSamples) { reset(); }

	void reset() {
		N = 0;
		samplingRate = 1.0;
		sum = T();
		sumSQ = T();
	};

	void addSample(const T& x) {
		if (!N) {
			minSample = maxSample = x;
		} else {
			if (x < minSample)
				minSample = x;
			if (maxSample < x)
				maxSample = x;
		}
		sum += x;
		sumSQ += x * x;
		N++;
		if (deterministicRandom()->random01() < samplingRate) {
			samples.push_back(x);
			if (samples.size() == minSamples * 2) {
				deterministicRandom()->randomShuffle(samples);
				samples.resize(minSamples);
				samplingRate /= 2;
			}
		}
	}
	//	void addHistogram(const Histrogram& h2);

	T mean() const { return sum * (1.0 / N); } // exact
	const T& min() const { return minSample; }
	const T& max() const { return maxSample; }
	T stdDev() const {
		if (!N)
			return T();
		return sqrt((sumSQ * N - sum * sum) * (1.0 / (N * (N - 1))));
	}
	T percentileEstimate(double p) {
		ASSERT(p <= 1 && p >= 0);
		int size = samples.size();
		if (!size)
			return T();
		if (size == 1)
			return samples[0];
		std::sort(samples.begin(), samples.end());
		double fi = p * double(size - 1);
		int li = p * double(size - 1);
		if (li == size - 1)
			return samples.back();
		double alpha = fi - li;
		return samples[li] * (1 - alpha) + samples[li + 1] * alpha;
	}
	T medianEstimate() { return percentileEstimate(0.5); }
	uint64_t samplesCount() const { return N; }

private:
	int minSamples;
	double samplingRate;
	std::vector<T> samples;
	T minSample;
	T maxSample;
	T sum;
	T sumSQ;
	uint64_t N;
};

struct KVTest {
	IKeyValueStore* store;
	Version startVersion;
	Version lastSet;
	Version lastCommit;
	Version lastDurable;
	Map<Key, IndexedSet<Version, NoMetric>> allSets;
	int nodeCount, keyBytes;
	bool dispose;

	explicit KVTest(int nodeCount, bool dispose, int keyBytes)
	  : store(nullptr), startVersion(Version(time(nullptr)) << 30), lastSet(startVersion), lastCommit(startVersion),
	    lastDurable(startVersion), nodeCount(nodeCount), keyBytes(keyBytes), dispose(dispose) {}
	~KVTest() { close(); }
	void close() {
		if (store) {
			TraceEvent("KVTestDestroy").log();
			if (dispose)
				store->dispose();
			else
				store->close();
			store = 0;
		}
	}

	Version get(KeyRef key, Version version) {
		auto s = allSets.find(key);
		if (s == allSets.end())
			return startVersion;
		auto& sets = s->value;
		auto it = sets.lastLessOrEqual(version);
		return it != sets.end() ? *it : startVersion;
	}
	void set(KeyValueRef kv) {
		store->set(kv);
		auto s = allSets.find(kv.key);
		if (s == allSets.end()) {
			allSets.insert(MapPair<Key, IndexedSet<Version, NoMetric>>(Key(kv.key), IndexedSet<Version, NoMetric>()));
			s = allSets.find(kv.key);
		}
		s->value.insert(lastSet, NoMetric());
	}

	Key randomKey() { return makeKey(deterministicRandom()->randomInt(0, nodeCount)); }
	Key makeKey(Version value) {
		Key k;
		((KeyRef&)k) = KeyRef(new (k.arena()) uint8_t[keyBytes], keyBytes);
		memcpy((uint8_t*)k.begin(), doubleToTestKey(value).begin(), 16);
		memset((uint8_t*)k.begin() + 16, '.', keyBytes - 16);
		return k;
	}
};

ACTOR Future<Void> testKVRead(KVTest* test, Key key, TestHistogram<float>* latency, PerfIntCounter* count) {
	// state Version s1 = test->lastCommit;
	state Version s2 = test->lastDurable;

	state double begin = timer();
	Optional<Value> val = wait(test->store->readValue(key));
	latency->addSample(timer() - begin);
	++*count;
	Version v = val.present() ? BinaryReader::fromStringRef<Version>(val.get(), Unversioned()) : test->startVersion;
	if (v < test->startVersion)
		v = test->startVersion; // ignore older data from the database

	// ASSERT( s1 <= v || test->get(key, s1)==v );  // Plan A
	ASSERT(s2 <= v || test->get(key, s2) == v); // Causal consistency
	ASSERT(v <= test->lastCommit); // read committed
	// ASSERT( v <= test->lastSet );  // read uncommitted
	return Void();
}

ACTOR Future<Void> testKVReadSaturation(KVTest* test, TestHistogram<float>* latency, PerfIntCounter* count) {
	while (true) {
		state double begin = timer();
		Optional<Value> val = wait(test->store->readValue(test->randomKey()));
		latency->addSample(timer() - begin);
		++*count;
		wait(delay(0));
	}
}

ACTOR Future<Void> testKVCommit(KVTest* test, TestHistogram<float>* latency, PerfIntCounter* count) {
	state Version v = test->lastSet;
	test->lastCommit = v;
	state double begin = timer();
	wait(test->store->commit());
	++*count;
	latency->addSample(timer() - begin);
	test->lastDurable = std::max(test->lastDurable, v);
	return Void();
}

Future<Void> testKVStore(struct KVStoreTestWorkload* const&);

struct KVStoreTestWorkload : TestWorkload {
	static constexpr auto NAME = "KVStoreTest";
	bool enabled, saturation;
	double testDuration, operationsPerSecond;
	double commitFraction, setFraction;
	int nodeCount, keyBytes, valueBytes;
	bool doSetup, doClear, doCount;
	std::string filename;
	PerfIntCounter reads, sets, commits;
	TestHistogram<float> readLatency, commitLatency;
	double setupTook;
	KeyValueStoreType storeType;

	KVStoreTestWorkload(WorkloadContext const& wcx)
	  : TestWorkload(wcx), reads("Reads"), sets("Sets"), commits("Commits"), setupTook(0) {
		enabled = !clientId; // only do this on the "first" client
		testDuration = getOption(options, "testDuration"_sr, 10.0);
		operationsPerSecond = getOption(options, "operationsPerSecond"_sr, 100e3);
		commitFraction = getOption(options, "commitFraction"_sr, .001);
		setFraction = getOption(options, "setFraction"_sr, .1);
		nodeCount = getOption(options, "nodeCount"_sr, 100000);
		keyBytes = getOption(options, "keyBytes"_sr, 8);
		valueBytes = getOption(options, "valueBytes"_sr, 8);
		doSetup = getOption(options, "setup"_sr, false);
		doClear = getOption(options, "clear"_sr, false);
		doCount = getOption(options, "count"_sr, false);
		filename = getOption(options, "filename"_sr, Value()).toString();
		saturation = getOption(options, "saturation"_sr, false);
		storeType = KeyValueStoreType::fromString(getOption(options, "storeType"_sr, "ssd"_sr).toString());
	}
	Future<Void> setup(Database const& cx) override { return Void(); }
	Future<Void> start(Database const& cx) override {
		if (enabled)
			return testKVStore(this);
		return Void();
	}
	Future<bool> check(Database const& cx) override { return true; }
	void metricsFromHistogram(std::vector<PerfMetric>& m, std::string name, TestHistogram<float>& h) const {
		m.emplace_back("Min " + name, 1000.0 * h.min(), Averaged::True);
		m.emplace_back("Average " + name, 1000.0 * h.mean(), Averaged::True);
		m.emplace_back("Median " + name, 1000.0 * h.medianEstimate(), Averaged::True);
		m.emplace_back("95%% " + name, 1000.0 * h.percentileEstimate(0.95), Averaged::True);
		m.emplace_back("Max " + name, 1000.0 * h.max(), Averaged::True);
	}
	void getMetrics(std::vector<PerfMetric>& m) override {
		if (setupTook)
			m.emplace_back("SetupTook", setupTook, Averaged::False);

		m.push_back(reads.getMetric());
		m.push_back(sets.getMetric());
		m.push_back(commits.getMetric());
		metricsFromHistogram(m, "Read Latency (ms)", readLatency);
		metricsFromHistogram(m, "Commit Latency (ms)", commitLatency);
	}
};

WorkloadFactory<KVStoreTestWorkload> KVStoreTestWorkloadFactory;

ACTOR Future<Void> testKVStoreMain(KVStoreTestWorkload* workload, KVTest* ptest) {
	state KVTest& test = *ptest;
	state ActorCollectionNoErrors ac;
	state std::deque<Future<Void>> reads;
	state BinaryWriter wr(Unversioned());
	state int64_t commitsStarted = 0;
	// test.store = makeDummyKeyValueStore();
	state int extraBytes = workload->valueBytes - sizeof(test.lastSet);
	state int i;
	ASSERT(extraBytes >= 0);
	state char* extraValue = new char[extraBytes];
	memset(extraValue, '.', extraBytes);

	if (workload->doCount) {
		state int64_t count = 0;
		state Key k;
		state double cst = timer();
		while (true) {
			RangeResult kv = wait(test.store->readRange(KeyRangeRef(k, "\xff\xff\xff\xff"_sr), 1000));
			count += kv.size();
			if (kv.size() < 1000)
				break;
			k = keyAfter(kv[kv.size() - 1].key);
		}
		double elapsed = timer() - cst;
		TraceEvent("KVStoreCount").detail("Count", count).detail("Took", elapsed);
		fmt::print("Counted: {0} in {1:0.1f}s\n", count, elapsed);
	}

	if (workload->doSetup) {
		wr << Version(0);
		wr.serializeBytes(extraValue, extraBytes);

		printf("Building %d nodes: ", workload->nodeCount);
		state double setupBegin = timer();
		state Future<Void> lastCommit = Void();
		for (i = 0; i < workload->nodeCount; i++) {
			test.store->set(KeyValueRef(test.makeKey(i), wr.toValue()));
			if (!((i + 1) % 10000) || i + 1 == workload->nodeCount) {
				wait(lastCommit);
				lastCommit = test.store->commit();
				printf("ETA: %f seconds\n", (timer() - setupBegin) / i * (workload->nodeCount - i));
			}
		}
		wait(lastCommit);
		workload->setupTook = timer() - setupBegin;
		TraceEvent("KVStoreSetup").detail("Count", workload->nodeCount).detail("Took", workload->setupTook);
	}

	state double t = now();
	state double stopAt = t + workload->testDuration;
	if (workload->saturation) {
		if (workload->commitFraction) {
			while (now() < stopAt) {
				for (int s = 0; s < 1 / workload->commitFraction; s++) {
					++test.lastSet;
					BinaryWriter wr(Unversioned());
					wr << test.lastSet;
					wr.serializeBytes(extraValue, extraBytes);
					test.set(KeyValueRef(test.randomKey(), wr.toValue()));
					++workload->sets;
				}
				++commitsStarted;
				wait(testKVCommit(&test, &workload->commitLatency, &workload->commits));
			}
		} else {
			std::vector<Future<Void>> actors;
			actors.reserve(100);
			for (int a = 0; a < 100; a++)
				actors.push_back(testKVReadSaturation(&test, &workload->readLatency, &workload->reads));
			wait(timeout(waitForAll(actors), workload->testDuration, Void()));
		}
	} else {
		while (t < stopAt) {
			double end = now();
			loop {
				t += 1.0 / workload->operationsPerSecond;
				double op = deterministicRandom()->random01();
				if (op < workload->commitFraction) {
					// Commit
					if (workload->commits.getValue() == commitsStarted) {
						++commitsStarted;
						ac.add(testKVCommit(&test, &workload->commitLatency, &workload->commits));
					}
				} else if (op < workload->commitFraction + workload->setFraction) {
					// Set
					++test.lastSet;
					BinaryWriter wr(Unversioned());
					wr << test.lastSet;
					wr.serializeBytes(extraValue, extraBytes);
					test.set(KeyValueRef(test.randomKey(), wr.toValue()));
					++workload->sets;
				} else {
					// Read
					ac.add(testKVRead(&test, test.randomKey(), &workload->readLatency, &workload->reads));
				}
				if (t >= end)
					break;
			}
			wait(delayUntil(t));
		}
	}

	if (workload->doClear) {
		state int chunk = 1000000;
		t = timer();
		for (i = 0; i < workload->nodeCount; i += chunk) {
			test.store->clear(KeyRangeRef(test.makeKey(i), test.makeKey(i + chunk)));
			wait(test.store->commit());
		}
		TraceEvent("KVStoreClear").detail("Took", timer() - t);
	}

	return Void();
}

ACTOR Future<Void> testKVStore(KVStoreTestWorkload* workload) {
	state KVTest test(workload->nodeCount, !workload->filename.size(), workload->keyBytes);
	state Error err;

	// wait( delay(1) );
	TraceEvent("GO").log();

	UID id = deterministicRandom()->randomUniqueID();
	std::string fn = workload->filename.size() ? workload->filename : id.toString();
	if (workload->storeType == KeyValueStoreType::SSD_BTREE_V2) {
		test.store = keyValueStoreSQLite(fn, id, KeyValueStoreType::SSD_BTREE_V2);
	} else if (workload->storeType == KeyValueStoreType::SSD_BTREE_V1) {
		test.store = keyValueStoreSQLite(fn, id, KeyValueStoreType::SSD_BTREE_V1);
	} else if (workload->storeType == KeyValueStoreType::SSD_REDWOOD_V1) {
		test.store = keyValueStoreRedwoodV1(fn, id);
	} else if (workload->storeType == KeyValueStoreType::SSD_ROCKSDB_V1) {
		test.store = keyValueStoreRocksDB(fn, id, KeyValueStoreType::SSD_ROCKSDB_V1);
	} else if (workload->storeType == KeyValueStoreType::SSD_SHARDED_ROCKSDB) {
		test.store = keyValueStoreRocksDB(
		    fn, id, KeyValueStoreType::SSD_SHARDED_ROCKSDB); // TODO: to replace the KVS in the future
	} else if (workload->storeType == KeyValueStoreType::MEMORY) {
		test.store = keyValueStoreMemory(fn, id, 500e6);
	} else if (workload->storeType == KeyValueStoreType::MEMORY_RADIXTREE) {
		test.store = keyValueStoreMemory(fn, id, 500e6, "fdr", KeyValueStoreType::MEMORY_RADIXTREE);
	} else {
		ASSERT(false);
	}

	wait(test.store->init());

	state Future<Void> main = testKVStoreMain(workload, &test);
	try {
		choose {
			when(wait(main)) {}
			when(wait(test.store->getError())) {
				ASSERT(false);
			}
		}
	} catch (Error& e) {
		err = e;
	}
	main.cancel();

	Future<Void> c = test.store->onClosed();
	test.close();
	wait(c);
	if (err.code() != invalid_error_code)
		throw err;
	return Void();
}
