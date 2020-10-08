/*
 * HealthMonitor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
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

#ifndef FDBRPC_HEALTH_MONITOR_H
#define FDBRPC_HEALTH_MONITOR_H

#include <deque>
#include <unordered_map>

#include "flow/flow.h"
// #include "fdbrpc/FailureMonitor.h"


template <class Entry>
class SlidingWindowStat {
public:
	SlidingWindowStat(const int windowDurationSecs_) : windowDurationSecs(windowDurationSecs_) {}

	void add(const Entry& val) {
		sweep();
		entries.emplace_back(now(), val);
	}

	int count() {
		sweep();
		return entries.size();
	}

private:
	void sweep() {
		const double CRITERIA = now() - windowDurationSecs;
		while(!entries.empty() && entries.front().first < CRITERIA) entries.pop_front();
	}

	std::deque<std::pair<double, Entry>> entries;

	// Size of sliding window in seconds. Older entries are purged.
	const int windowDurationSecs;
};

class ClosedConnectionsStats {
public:
	void add(const NetworkAddress& address) {
		auto it = counters.find(address);
		if (counters.find(address) == counters.end()) {
			std::tie(it, std::ignore) = counters.insert(std::make_pair(
			    address, SlidingWindowStat<int>(FLOW_KNOBS->HEALTH_MONITOR_CLIENT_REQUEST_INTERVAL_SECS)));
		}
		it->second.add(1);
	}

	int count(const NetworkAddress& address) {
		auto it = counters.find(address);
		if (it == counters.end()) {
			return 0;
		}

		return it->second.count();
	}

	bool limitExceeded(const NetworkAddress& address) {
		return count(address) > FLOW_KNOBS->HEALTH_MONITOR_CONNECTION_MAX_CLOSED;
	}

private:
	std::unordered_map<NetworkAddress, SlidingWindowStat<int>> counters;
};

namespace {
template <typename PEER_TYPE, typename LATENCY_TYPE>
class LatencyBase {
	using SlidingWindowState_t = SlidingWindowStat<LATENCY_TYPE>;

public:
	using peer_t = PEER_TYPE
	using latency_t = LATENCY_TYPE

	void add(const peer_t& peer, const latency_t& latency) {
		if (latencies.find(peer) == latencies.end()) {
			latencies[peer] = SlidingWindowState_t(FLOW_KNOBS->HEALTH_MONITOR_CLIENT_REQUEST_INTERVAL_SECS);
		}
		latencies[peer].add(latency);
	}

protected:
	std::unordered_map<peer_t, SlidingWindowStat<double>> latencies;
};

template <typename LATENCY_TYPE>
using NetworkLatencyBase = LatencyBase<NetworkAddress, LATENCY_TYPE>;
}	// anonymous namespace

using TLogPushLatency = NetworkLatencyBase<double>;
using CommitResolvingLatency = NetworkLatencyBase<double>;

struct FailureMonitorMetrics {
	// Is this peer marked as failed for N seconds.
	bool failed;

	// Number of times the connection failed in last N seconds.
	int failedConnectionCount;

	// Number of slow and total replies from tLog, i.e. `pair<SlowReplies, TotalReplies>`.
	Optional<TLogPushLatency> tlogPushLatencies;

	Optional<CommitResolvingLatency> commitResolvingLatencies;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, failed, failedConnectionCount, tlogPushLatencies, commitResolvingLatencies);
	}
};

struct HealthMonitor {
	ClosedConnectionsStats closedConnections;

	FailureMonitorMetrics aggregateFailureMetrics(const NetworkAddress& peer) {
		FailureMonitorMetrics metrics;
		// metrics.failed = IFailureMonitor::failureMonitor().getState(peer).isFailed();
		metrics.failedConnectionCount = closedConnections.count(peer);

		return metrics;
	}
};

#endif // FDBRPC_HEALTH_MONITOR_H
