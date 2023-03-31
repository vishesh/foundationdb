import Flow
import flow_swift
@preconcurrency import FDBServer
import FDBClient
import fdbclient_swift
import CxxStdlib


extension NetworkAddressSet.const_iterator : Equatable {
    public static func==(lhs: NetworkAddressSet.const_iterator, rhs: NetworkAddressSet.const_iterator) -> Bool {
        unsafeBitCast(lhs, to: UnsafeRawPointer.self) == unsafeBitCast(rhs, to: UnsafeRawPointer.self)
    }
}
extension NetworkAddressSet.const_iterator : UnsafeCxxInputIterator {}
extension NetworkAddressSet: CxxSequence {}

extension NetworkWorkerHealthMap.const_iterator : Equatable {
    public static func==(lhs: NetworkWorkerHealthMap.const_iterator, rhs: NetworkWorkerHealthMap.const_iterator) -> Bool {
        unsafeBitCast(lhs, to: UnsafeRawPointer.self) == unsafeBitCast(rhs, to: UnsafeRawPointer.self)
    }
}
extension NetworkWorkerHealthMap.const_iterator : UnsafeCxxInputIterator {}
extension NetworkWorkerHealthMap: CxxSequence {
  public typealias Element = NetworkWorkerHealthMap.const_iterator.Pointee
}


extension DegradedTimesMap.const_iterator : Equatable {
    public static func==(lhs: DegradedTimesMap.const_iterator, rhs: DegradedTimesMap.const_iterator) -> Bool {
        unsafeBitCast(lhs, to: UnsafeRawPointer.self) == unsafeBitCast(rhs, to: UnsafeRawPointer.self)
    }
}
extension DegradedTimesMap.const_iterator : UnsafeCxxInputIterator {}
extension DegradedTimesMap: CxxSequence {
  public typealias Element = DegradedTimesMap.const_iterator.Pointee
}


@_expose(Cxx)
public func updateWorkerHealth(req: UpdateWorkerHealthRequest, myself: inout ClusterControllerData ) {
    var degradedPeersString = "";
    for i in 0..<req.degradedPeers.size() {
        degradedPeersString += (i == 0 ? "" : " ") + String(req.degradedPeers[i].toString())
    }

    var disconnectedPeersString = "";
    for i in 0..<req.disconnectedPeers.size() {
        degradedPeersString += (i == 0 ? "" : " ") + String(req.disconnectedPeers[i].toString())
    }

    // TraceEvent("ClusterControllerUpdateWorkerHealth")
    //   .detail("WorkerAddress", req.address)
    //   .detail("DegradedPeers", degradedPeersString)
    //   .detail("DisconnectedPeers", disconnectedPeersString)

    let currentTime = now()

    // Current `workerHealth` doesn't have any information about the incoming worker. Add the worker into
	// `workerHealth`.
    if myself.workerHealth[req.address] == nil {
        myself.insertWorkerHealth(req.address)
        for degradedPeer in req.degradedPeers {
            let dt = WorkerHealth.DegradedTimes(startTime: currentTime, lastRefreshTime: currentTime);
            myself.updateWorkerHealthDegradedPeer(req.address, degradedPeer, dt)
        }

        for disconnectedPeer in req.disconnectedPeers {
            let dt = WorkerHealth.DegradedTimes(startTime: currentTime, lastRefreshTime: currentTime);
            myself.updateWorkerHealthDisconnectedPeer(req.address, disconnectedPeer, dt)
        }
        return
    }

    myself.insertWorkerHealth(req.address)

    for peer in req.degradedPeers {
        guard let wh = myself.workerHealth[req.address] else { continue }
        if var dt = wh.degradedPeers[peer] {
            dt.lastRefreshTime = currentTime
            myself.updateWorkerHealthDegradedPeer(req.address, peer, dt)
        }  else {
            let dt = WorkerHealth.DegradedTimes(startTime: currentTime, lastRefreshTime: currentTime);
            myself.updateWorkerHealthDegradedPeer(req.address, peer, dt)
        }
    }

    for peer in req.disconnectedPeers {
        guard let wh = myself.workerHealth[req.address] else { continue }
        if var dt = wh.disconnectedPeers[peer] {
            dt.lastRefreshTime = currentTime
            myself.updateWorkerHealthDisconnectedPeer(req.address, peer, dt)
        }  else {
            let dt = WorkerHealth.DegradedTimes(startTime: currentTime, lastRefreshTime: currentTime);
            myself.updateWorkerHealthDisconnectedPeer(req.address, peer, dt)
        }
    }
}

@_expose(Cxx)
public func workerHealthMonitorSwift(ccData: inout ClusterControllerData) {
    var toErase: [NetworkAddress] = []
    for excluded in ccData.excludedDegradedServers {
        if !ccData.degradationInfo.degradedServers.contains(excluded) {
            toErase.append(excluded)
        }
    }

    for e in toErase {
        ccData.excludedDegradedServers.erase(e)
    }

    if !ccData.degradationInfo.degradedServers.empty() || ccData.degradationInfo.degradedSatellite {
        var degradedServerString = ""
        for server in ccData.degradationInfo.degradedServers {
            degradedServerString += "\(server.toString()) "
        }

        // TraceEvent("ClusterControllerHealthMonitor", UID())
        //    .detail("DegradedServers", degradedServerString)
        //    .detail("DegradedSatellite", ccData.degradationInfo.degradedSatellite)

        if ccData.shouldTriggerRecoveryDueToDegradedServers() {
            if (getServerKnobs().CC_HEALTH_TRIGGER_RECOVERY) {
                if ccData.recentRecoveryCountDueToHealth() < getServerKnobs().CC_MAX_HEALTH_RECOVERY_COUNT {
                    ccData.recentHealthTriggeredRecoveryTime.push(now())
                    ccData.excludedDegradedServers = ccData.degradationInfo.degradedServers
                    // TraceEvent("DegradedServerDetectedAndTriggerRecovery")
					// .detail("RecentRecoveryCountDueToHealth", self->recentRecoveryCountDueToHealth());
                    ccData.db.getForceMasterFailureTrigger().trigger()
                }
            } else {
                ccData.excludedDegradedServers.clear()
                // TraceEvent("DegradedServerDetectedAndSuggestRecovery").log()
            }
        } else if (ccData.shouldTriggerFailoverDueToDegradedServers())  {
            let ccUpTime = now() - machineStartTime()
            if (getServerKnobs().CC_HEALTH_TRIGGER_FAILOVER && ccUpTime > getServerKnobs().INITIAL_UPDATE_CROSS_DC_INFO_DELAY) {
                // TraceEvent("DegradedServerDetectedAndTriggerFailover").log()
                if let ccDcId = Swift.Optional(cxxOptional: ccData.clusterControllerDcId) {
                    let remoteDcId = isEqualStandaloneStringRef(ccData.db.config.regions[0].dcId, ccDcId) ? ccData.db.config.regions[1].dcId : ccData.db.config.regions[0].dcId
                    var dcPriority : VecOptKey = VecOptKey()
                    dcPriority.push_back(OptionalKey(remoteDcId))
                    dcPriority.push_back(ccData.clusterControllerDcId)
                    ccData.getDesiredDcIds().set(OptVecOptKey(dcPriority))
                    return
                }
            } else {
                // TraceEvent("DegradedServerDetectedAndSuggestFailover").detail("CCUpTime", ccUpTime)
            }
        }
    }
}
