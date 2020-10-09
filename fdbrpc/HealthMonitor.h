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

struct FailureMonitorMetrics {
	// Is this peer marked as failed for N seconds.
	bool failed;

	// Number of times the connection failed in last N seconds.
	int failedConnectionCount;

	// Number of slow and total replies from tLog, i.e. `pair<SlowReplies, TotalReplies>`.
	Optional<std::pair<int, int>> tlogPushLatencies;

	// Last update. Doesn't need to be serialized for now, as its local time which may be
	// useless for CC.
	double lastUpdated;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, failed, failedConnectionCount, tlogPushLatencies);
	}
};

template <class Entry>
class SlidingWindowStat {
public:
	SlidingWindowStat(int windowDurationSecs) : windowDurationSecs(windowDurationSecs) {}

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
		for (const auto& it : entries) {
			if (it.first < now() - windowDurationSecs) {
				entries.pop_front();
			}
		}
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

class TLogPushLatencies {
public:
	void add(const NetworkAddress& address, double latency) {
		auto it = latencies.find(address);
		if (latencies.find(address) == latencies.end()) {
			std::tie(it, std::ignore) = latencies.insert(std::make_pair(
			    address, SlidingWindowStat<double>(FLOW_KNOBS->HEALTH_MONITOR_CLIENT_REQUEST_INTERVAL_SECS)));
		}
		it->second.add(1);
	}

private:
	double average;
	
	std::unordered_map<NetworkAddress, SlidingWindowStat<double>> latencies;
};

struct HealthMonitor {
	ClosedConnectionsStats closedConnections;

	TLogPushLatencies tLogPushLatencies;

	FailureMonitorMetrics aggregateFailureMetrics(const NetworkAddress& peer) {
		FailureMonitorMetrics metrics;
		// metrics.failed = IFailureMonitor::failureMonitor().getState(peer).isFailed();
		metrics.failedConnectionCount = closedConnections.count(peer);

		return metrics;
	}
};

#endif // FDBRPC_HEALTH_MONITOR_H
