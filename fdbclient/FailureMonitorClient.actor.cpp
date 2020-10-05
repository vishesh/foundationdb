/*
 * FailureMonitorClient.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
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

#include "fdbclient/FailureMonitorClient.h"
#include "fdbclient/ClusterInterface.h"
#include "fdbrpc/FailureMonitor.h"
#include "fdbrpc/HealthMonitor.h"

ACTOR Future<Void> failureMonitorStatsPublisherLoop(ClusterInterface controller) {
	state Version version = 0;
	state double nextDelay = 0;
	state double startTime = 0;
	state HealthMonitor* healthMonitor = FlowTransport::transport().healthMonitor();

	try {
		loop {
			FailureMonitorPublishMetricsRequest req;
			std::unordered_set<NetworkAddress> peerList = FlowTransport::transport().getPeerList();
			for (const auto& peer : peerList) {
				req.metrics[peer] = healthMonitor->aggregateFailureMetrics(peer);

				// TODO (Vishesh) Fix the compile error when dont this in HealthMonitor
				req.metrics[peer].failed = IFailureMonitor::failureMonitor().getState(peer).isFailed();
			}

			startTime = now();
			choose {
				when(FailureMonitorPublishMetricsReply reply = wait(controller.failureMonitoring.getReply(req))) {
					TraceEvent("FailureMonitorClientPublishMetrics");
					nextDelay =
					    std::max(CLIENT_KNOBS->FAILURE_MONITOR_PUBLISH_INTERVAL_SECS - (now() - startTime), 0.0);
				}
				when(wait(delay(CLIENT_KNOBS->FAILURE_MONITOR_PUBLISH_REQUEST_TIMEOUT_SECS))) {
					TraceEvent("FailureMonitorClientPublishMetricsTimedOut").detail("Elapsed", now() - startTime);
					nextDelay = CLIENT_KNOBS->FAILURE_MONITOR_PUBLISH_RETRY_INTERVAL_SECS;
				}
			}

			wait(delayJittered(nextDelay));
		}
	} catch (Error& e) {
		if (e.code() == error_code_broken_promise) {
			// broken_promise from clustercontroller means it has died (and hopefully will be replaced)
			return Void();
		}
		TraceEvent(SevError, "FailureMonitorPublishStatsError").error(e);
		throw;
	}
}

ACTOR Future<Void> failureMonitorStatsPublisher(Reference<AsyncVar<Optional<struct ClusterInterface>>> ci) {
	if (CLIENT_KNOBS->FAILURE_MONITOR_PUBLISH_TO_CC_ENABLED) {
		return Never();
	}

	loop {
		state Future<Void> client = ci->get().present() ? failureMonitorStatsPublisherLoop(ci->get().get()) : Void();
		wait(ci->onChange());
	}
}
