/*
 * ClusterController.actor.cpp
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

#include "fdbrpc/FailureMonitor.h"
#include "flow/ActorCollection.h"
#include "fdbclient/NativeAPI.h"
#include "fdbserver/CoordinationInterface.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/MoveKeys.h"
#include "fdbserver/WorkerInterface.h"
#include "fdbserver/LeaderElection.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/ClusterRecruitmentInterface.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/Status.h"
#include <algorithm>
#include "fdbclient/DatabaseContext.h"
#include "fdbserver/RecoveryState.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbrpc/Replication.h"
#include "fdbrpc/ReplicationUtils.h"
#include "fdbclient/KeyBackedTypes.h"
#include "flow/Util.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

void failAfter( Future<Void> trigger, Endpoint e );

struct WorkerInfo : NonCopyable {
	Future<Void> watcher;
	ReplyPromise<RegisterWorkerReply> reply;
	Generation gen;
	int reboots;
	double lastAvailableTime;
	WorkerInterface interf;
	ProcessClass initialClass;
	ProcessClass processClass;
	ClusterControllerPriorityInfo priorityInfo;

	WorkerInfo() : gen(-1), reboots(0), lastAvailableTime(now()), priorityInfo(ProcessClass::UnsetFit, false, ClusterControllerPriorityInfo::FitnessUnknown) {}
	WorkerInfo( Future<Void> watcher, ReplyPromise<RegisterWorkerReply> reply, Generation gen, WorkerInterface interf, ProcessClass initialClass, ProcessClass processClass, ClusterControllerPriorityInfo priorityInfo ) :
		watcher(watcher), reply(reply), gen(gen), reboots(0), lastAvailableTime(now()), interf(interf), initialClass(initialClass), processClass(processClass), priorityInfo(priorityInfo) {}

	WorkerInfo( WorkerInfo&& r ) noexcept(true) : watcher(std::move(r.watcher)), reply(std::move(r.reply)), gen(r.gen),
		reboots(r.reboots), lastAvailableTime(r.lastAvailableTime), interf(std::move(r.interf)), initialClass(r.initialClass), processClass(r.processClass), priorityInfo(r.priorityInfo) {}
	void operator=( WorkerInfo&& r ) noexcept(true) {
		watcher = std::move(r.watcher);
		reply = std::move(r.reply);
		gen = r.gen;
		reboots = r.reboots;
		lastAvailableTime = r.lastAvailableTime;
		interf = std::move(r.interf);
		initialClass = r.initialClass;
		processClass = r.processClass;
		priorityInfo = r.priorityInfo;
	}
};

struct WorkerFitnessInfo {
	std::pair<WorkerInterface, ProcessClass> worker;
	ProcessClass::Fitness fitness;
	int used;

	WorkerFitnessInfo() : fitness(ProcessClass::NeverAssign), used(0) {}
	WorkerFitnessInfo(std::pair<WorkerInterface, ProcessClass> worker, ProcessClass::Fitness fitness, int used) : worker(worker), fitness(fitness), used(used) {}
};

class ClusterControllerData {
public:
	struct DBInfo {
		Reference<AsyncVar<ClientDBInfo>> clientInfo;
		Reference<AsyncVar<ServerDBInfo>> serverInfo;
		ProcessIssuesMap clientsWithIssues, workersWithIssues;
		std::map<NetworkAddress, double> incompatibleConnections;
		ClientVersionMap clientVersionMap;
		std::map<NetworkAddress, std::string> traceLogGroupMap;
		Promise<Void> forceMasterFailure;
		int64_t masterRegistrationCount;
		bool recoveryStalled;
		bool forceRecovery;
		DatabaseConfiguration config;   // Asynchronously updated via master registration
		DatabaseConfiguration fullyRecoveredConfig;
		Database db;
		int unfinishedRecoveries;
		int logGenerations;

		DBInfo() : masterRegistrationCount(0), recoveryStalled(false), forceRecovery(false), unfinishedRecoveries(0), logGenerations(0),
			clientInfo( new AsyncVar<ClientDBInfo>( ClientDBInfo() ) ),
			serverInfo( new AsyncVar<ServerDBInfo>( ServerDBInfo() ) ),
			db( DatabaseContext::create( clientInfo, Future<Void>(), LocalityData(), true, TaskDefaultEndpoint, true ) )  // SOMEDAY: Locality!
		{

		}
	};

	struct UpdateWorkerList {
		Future<Void> init( Database const& db ) {
			return update(this, db);
		}

		void set(Optional<Standalone<StringRef>> processID, Optional<ProcessData> data ) {
			delta[processID] = data;
			anyDelta.set(true);
		}

	private:
		std::map<Optional<Standalone<StringRef>>, Optional<ProcessData>> delta;
		AsyncVar<bool> anyDelta;

		ACTOR static Future<Void> update( UpdateWorkerList* self, Database db ) {
			// The Database we are using is based on worker registrations to this cluster controller, which come only
			// from master servers that we started, so it shouldn't be possible for multiple cluster controllers to fight.
			state Transaction tr(db);
			loop {
				try {
					tr.clear( workerListKeys );
					wait( tr.commit() );
					break;
				} catch (Error& e) {
					wait( tr.onError(e) );
				}
			}

			loop {
				// Wait for some changes
				while (!self->anyDelta.get())
					wait( self->anyDelta.onChange() );
				self->anyDelta.set(false);

				state std::map<Optional<Standalone<StringRef>>, Optional<ProcessData>> delta;
				delta.swap( self->delta );

				TraceEvent("UpdateWorkerList").detail("DeltaCount", delta.size());

				// Do a transaction to write the changes
				loop {
					try {
						for(auto w = delta.begin(); w != delta.end(); ++w) {
							if (w->second.present()) {
								tr.set( workerListKeyFor( w->first.get() ), workerListValue( w->second.get()) );
							} else
								tr.clear( workerListKeyFor( w->first.get() ) );
						}
						wait( tr.commit() );
						break;
					} catch (Error& e) {
						wait( tr.onError(e) );
					}
				}
			}
		}
	};

	bool workerAvailable( WorkerInfo const& worker, bool checkStable ) {
		return ( now() - startTime < 2 * FLOW_KNOBS->SERVER_REQUEST_INTERVAL ) || ( IFailureMonitor::failureMonitor().getState(worker.interf.storage.getEndpoint()).isAvailable() && ( !checkStable || worker.reboots < 2 ) );
	}

	std::pair<WorkerInterface, ProcessClass> getStorageWorker( RecruitStorageRequest const& req ) {
		std::set<Optional<Standalone<StringRef>>> excludedMachines( req.excludeMachines.begin(), req.excludeMachines.end() );
		std::set<Optional<Standalone<StringRef>>> includeDCs( req.includeDCs.begin(), req.includeDCs.end() );
		std::set<AddressExclusion> excludedAddresses( req.excludeAddresses.begin(), req.excludeAddresses.end() );

		for( auto& it : id_worker )
			if( workerAvailable( it.second, false ) &&
					!excludedMachines.count(it.second.interf.locality.zoneId()) &&
					( includeDCs.size() == 0 || includeDCs.count(it.second.interf.locality.dcId()) ) &&
					!addressExcluded(excludedAddresses, it.second.interf.address()) &&
					it.second.processClass.machineClassFitness( ProcessClass::Storage ) <= ProcessClass::UnsetFit ) {
				return std::make_pair(it.second.interf, it.second.processClass);
			}

		if( req.criticalRecruitment ) {
			ProcessClass::Fitness bestFit = ProcessClass::NeverAssign;
			Optional<std::pair<WorkerInterface, ProcessClass>> bestInfo;
			for( auto& it : id_worker ) {
				ProcessClass::Fitness fit = it.second.processClass.machineClassFitness( ProcessClass::Storage );
				if( workerAvailable( it.second, false ) &&
						!excludedMachines.count(it.second.interf.locality.zoneId()) &&
						( includeDCs.size() == 0 || includeDCs.count(it.second.interf.locality.dcId()) ) &&
						!addressExcluded(excludedAddresses, it.second.interf.address()) &&
						fit < bestFit ) {
					bestFit = fit;
					bestInfo = std::make_pair(it.second.interf, it.second.processClass);
				}
			}

			if( bestInfo.present() ) {
				return bestInfo.get();
			}
		}

		throw no_more_servers();
	}

	std::vector<std::pair<WorkerInterface, ProcessClass>> getWorkersForSeedServers( DatabaseConfiguration const& conf, IRepPolicyRef const& policy, Optional<Optional<Standalone<StringRef>>> const& dcId = Optional<Optional<Standalone<StringRef>>>() ) {
		std::map<ProcessClass::Fitness, vector<std::pair<WorkerInterface, ProcessClass>>> fitness_workers;
		std::vector<std::pair<WorkerInterface, ProcessClass>> results;
		LocalitySetRef logServerSet = Reference<LocalitySet>(new LocalityMap<std::pair<WorkerInterface, ProcessClass>>());
		LocalityMap<std::pair<WorkerInterface, ProcessClass>>* logServerMap = (LocalityMap<std::pair<WorkerInterface, ProcessClass>>*) logServerSet.getPtr();
		bool bCompleted = false;

		for( auto& it : id_worker ) {
			auto fitness = it.second.processClass.machineClassFitness( ProcessClass::Storage );
			if( workerAvailable(it.second, false) && !conf.isExcludedServer(it.second.interf.address()) && fitness != ProcessClass::NeverAssign && ( !dcId.present() || it.second.interf.locality.dcId()==dcId.get() ) ) {
				fitness_workers[ fitness ].push_back(std::make_pair(it.second.interf, it.second.processClass));
			}
		}

		for( auto& it : fitness_workers ) {
			for (auto& worker : it.second ) {
				logServerMap->add(worker.first.locality, &worker);
			}

			std::vector<LocalityEntry> bestSet;
			if( logServerSet->selectReplicas(policy, bestSet) ) {
				results.reserve(bestSet.size());
				for (auto& entry : bestSet) {
					auto object = logServerMap->getObject(entry);
					results.push_back(*object);
				}
				bCompleted = true;
				break;
			}
		}

		logServerSet->clear();
		logServerSet.clear();

		if (!bCompleted) {
			throw no_more_servers();
		}

		return results;
	}

	std::vector<std::pair<WorkerInterface, ProcessClass>> getWorkersForTlogs( DatabaseConfiguration const& conf, int32_t required, int32_t desired, IRepPolicyRef const& policy, std::map< Optional<Standalone<StringRef>>, int>& id_used, bool checkStable = false, std::set<Optional<Key>> dcIds = std::set<Optional<Key>>() ) {
		std::map<ProcessClass::Fitness, vector<std::pair<WorkerInterface, ProcessClass>>> fitness_workers;
		std::vector<std::pair<WorkerInterface, ProcessClass>> results;
		std::vector<LocalityData> unavailableLocals;
		LocalitySetRef logServerSet;
		LocalityMap<std::pair<WorkerInterface, ProcessClass>>* logServerMap;
		bool bCompleted = false;

		logServerSet = Reference<LocalitySet>(new LocalityMap<std::pair<WorkerInterface, ProcessClass>>());
		logServerMap = (LocalityMap<std::pair<WorkerInterface, ProcessClass>>*) logServerSet.getPtr();

		for( auto& it : id_worker ) {
			auto fitness = it.second.processClass.machineClassFitness( ProcessClass::TLog );
			if( workerAvailable(it.second, checkStable) && !conf.isExcludedServer(it.second.interf.address()) && fitness != ProcessClass::NeverAssign && (!dcIds.size() || dcIds.count(it.second.interf.locality.dcId())) ) {
				fitness_workers[ fitness ].push_back(std::make_pair(it.second.interf, it.second.processClass));
			}
			else {
				unavailableLocals.push_back(it.second.interf.locality);
			}
		}

		results.reserve(results.size() + id_worker.size());
		for (int fitness = ProcessClass::BestFit; fitness != ProcessClass::NeverAssign; fitness ++)
		{
			auto fitnessEnum = (ProcessClass::Fitness) fitness;
			if (fitness_workers.find(fitnessEnum) == fitness_workers.end())
				continue;
			for (auto& worker : fitness_workers[(ProcessClass::Fitness) fitness] ) {
				logServerMap->add(worker.first.locality, &worker);
			}
			if (logServerSet->size() < required) {
				TraceEvent(SevWarn,"GWFTADTooFew", id).detail("Fitness", fitness).detail("Processes", logServerSet->size()).detail("Required", required).detail("TLogPolicy", policy->info()).detail("DesiredLogs", desired);
			}
			else if (logServerSet->size() == required || logServerSet->size() <= desired) {
				if (logServerSet->validate(policy)) {
					for (auto& object : logServerMap->getObjects()) {
						results.push_back(*object);
					}
					bCompleted = true;
					break;
				}
				TraceEvent(SevWarn,"GWFTADNotAcceptable", id).detail("Fitness", fitness).detail("Processes", logServerSet->size()).detail("Required", required).detail("TLogPolicy",policy->info()).detail("DesiredLogs", desired);
			}
			// Try to select the desired size, if larger
			else {
				std::vector<LocalityEntry> bestSet;
				std::vector<LocalityData> tLocalities;
				
				// Try to find the best team of servers to fulfill the policy
				if (findBestPolicySet(bestSet, logServerSet, policy, desired, SERVER_KNOBS->POLICY_RATING_TESTS, SERVER_KNOBS->POLICY_GENERATIONS)) {
					results.reserve(results.size() + bestSet.size());
					for (auto& entry : bestSet) {
						auto object = logServerMap->getObject(entry);
						ASSERT(object);
						results.push_back(*object);
						tLocalities.push_back(object->first.locality);
					}
					TraceEvent("GWFTADBestResults", id).detail("Fitness", fitness).detail("Processes", logServerSet->size()).detail("BestCount", bestSet.size()).detail("BestZones", ::describeZones(tLocalities))
						.detail("BestDataHalls", ::describeDataHalls(tLocalities)).detail("TLogPolicy", policy->info()).detail("TotalResults", results.size()).detail("DesiredLogs", desired);
					bCompleted = true;
					break;
				}
				TraceEvent(SevWarn,"GWFTADNoBest", id).detail("Fitness", fitness).detail("Processes", logServerSet->size()).detail("Required", required).detail("TLogPolicy", policy->info()).detail("DesiredLogs", desired);
			}
		}

		// If policy cannot be satisfied
		if (!bCompleted) {
			std::vector<LocalityData> tLocalities;
			for (auto& object : logServerMap->getObjects()) {
				tLocalities.push_back(object->first.locality);
			}

			TraceEvent(SevWarn, "GetTLogTeamFailed").detail("Policy", policy->info()).detail("Processes", logServerSet->size()).detail("Workers", id_worker.size()).detail("FitnessGroups", fitness_workers.size())
				.detail("TLogZones", ::describeZones(tLocalities)).detail("TLogDataHalls", ::describeDataHalls(tLocalities)).detail("MissingZones", ::describeZones(unavailableLocals))
				.detail("MissingDataHalls", ::describeDataHalls(unavailableLocals)).detail("Required", required).detail("DesiredLogs", desired).detail("RatingTests",SERVER_KNOBS->POLICY_RATING_TESTS)
				.detail("CheckStable", checkStable).detail("PolicyGenerations",SERVER_KNOBS->POLICY_GENERATIONS).backtrace();

			logServerSet->clear();
			logServerSet.clear();
			throw no_more_servers();
		}

		for (auto& result : results) {
			id_used[result.first.locality.processId()]++;
		}

		TraceEvent("GetTLogTeamDone").detail("Completed", bCompleted).detail("Policy", policy->info()).detail("Results", results.size()).detail("Processes", logServerSet->size()).detail("Workers", id_worker.size())
			.detail("Required", required).detail("Desired", desired).detail("RatingTests",SERVER_KNOBS->POLICY_RATING_TESTS).detail("PolicyGenerations",SERVER_KNOBS->POLICY_GENERATIONS);

		logServerSet->clear();
		logServerSet.clear();

		return results;
	}

	//FIXME: This logic will fallback unnecessarily when usable dcs > 1 because it does not check all combinations of potential satellite locations
	std::vector<std::pair<WorkerInterface, ProcessClass>> getWorkersForSatelliteLogs( const DatabaseConfiguration& conf, const RegionInfo& region, std::map< Optional<Standalone<StringRef>>, int>& id_used, bool& satelliteFallback, bool checkStable = false ) {
		int startDC = 0;
		loop {
			if(startDC > 0 && startDC >= region.satellites.size() + 1 - (satelliteFallback ? region.satelliteTLogUsableDcsFallback : region.satelliteTLogUsableDcs)) {
				if(satelliteFallback || region.satelliteTLogUsableDcsFallback == 0) {
					throw no_more_servers();
				} else {
					if(now() - startTime < SERVER_KNOBS->WAIT_FOR_GOOD_RECRUITMENT_DELAY) {
						throw operation_failed();
					}
					satelliteFallback = true;
					startDC = 0;
				}
			}

			try {
				std::set<Optional<Key>> satelliteDCs;
				for(int s = startDC; s < std::min<int>(startDC + (satelliteFallback ? region.satelliteTLogUsableDcsFallback : region.satelliteTLogUsableDcs), region.satellites.size()); s++) {
					satelliteDCs.insert(region.satellites[s].dcId);
				}

				if(satelliteFallback) {
					return getWorkersForTlogs( conf, region.satelliteTLogReplicationFactorFallback, conf.getDesiredSatelliteLogs(region.dcId)*region.satelliteTLogUsableDcsFallback/region.satelliteTLogUsableDcs, region.satelliteTLogPolicyFallback, id_used, checkStable, satelliteDCs );
				} else {
					return getWorkersForTlogs( conf, region.satelliteTLogReplicationFactor, conf.getDesiredSatelliteLogs(region.dcId), region.satelliteTLogPolicy, id_used, checkStable, satelliteDCs );
				}
			} catch (Error &e) {
				if(e.code() != error_code_no_more_servers) {
					throw;
				}
			}

			startDC++;
		}
	}

	WorkerFitnessInfo getWorkerForRoleInDatacenter(Optional<Standalone<StringRef>> const& dcId, ProcessClass::ClusterRole role, ProcessClass::Fitness unacceptableFitness, DatabaseConfiguration const& conf, std::map< Optional<Standalone<StringRef>>, int>& id_used, bool checkStable = false ) {
		std::map<std::pair<ProcessClass::Fitness,int>, vector<std::pair<WorkerInterface, ProcessClass>>> fitness_workers;

		for( auto& it : id_worker ) {
			auto fitness = it.second.processClass.machineClassFitness( role );
			if(conf.isExcludedServer(it.second.interf.address())) {
				fitness = std::max(fitness, ProcessClass::ExcludeFit);
			}
			if( workerAvailable(it.second, checkStable) && fitness < unacceptableFitness && it.second.interf.locality.dcId()==dcId ) {
				fitness_workers[ std::make_pair(fitness, id_used[it.first]) ].push_back(std::make_pair(it.second.interf, it.second.processClass));
			}
		}

		for( auto& it : fitness_workers ) {
			auto& w = it.second;
			g_random->randomShuffle(w);
			for( int i=0; i < w.size(); i++ ) {
				id_used[w[i].first.locality.processId()]++;
				return WorkerFitnessInfo(w[i], it.first.first, it.first.second);
			}
		}

		throw no_more_servers();
	}

	vector<std::pair<WorkerInterface, ProcessClass>> getWorkersForRoleInDatacenter(Optional<Standalone<StringRef>> const& dcId, ProcessClass::ClusterRole role, int amount, DatabaseConfiguration const& conf, std::map< Optional<Standalone<StringRef>>, int>& id_used, Optional<WorkerFitnessInfo> minWorker = Optional<WorkerFitnessInfo>(), bool checkStable = false ) {
		std::map<std::pair<ProcessClass::Fitness,int>, vector<std::pair<WorkerInterface, ProcessClass>>> fitness_workers;
		vector<std::pair<WorkerInterface, ProcessClass>> results;
		if (amount <= 0)
			return results;

		for( auto& it : id_worker ) {
			auto fitness = it.second.processClass.machineClassFitness( role );
			if( workerAvailable(it.second, checkStable) && !conf.isExcludedServer(it.second.interf.address()) && it.second.interf.locality.dcId() == dcId &&
			  ( !minWorker.present() || ( it.second.interf.id() != minWorker.get().worker.first.id() && ( fitness < minWorker.get().fitness || (fitness == minWorker.get().fitness && id_used[it.first] <= minWorker.get().used ) ) ) ) ) {
				fitness_workers[ std::make_pair(fitness, id_used[it.first]) ].push_back(std::make_pair(it.second.interf, it.second.processClass));
			}
		}

		for( auto& it : fitness_workers ) {
			auto& w = it.second;
			g_random->randomShuffle(w);
			for( int i=0; i < w.size(); i++ ) {
				results.push_back(w[i]);
				id_used[w[i].first.locality.processId()]++;
				if( results.size() == amount )
					return results;
			}
		}

		return results;
	}

	struct RoleFitness {
		ProcessClass::Fitness bestFit;
		ProcessClass::Fitness worstFit;
		int count;

		RoleFitness(int bestFit, int worstFit, int count) : bestFit((ProcessClass::Fitness)bestFit), worstFit((ProcessClass::Fitness)worstFit), count(count) {}

		RoleFitness(int fitness, int count) : bestFit((ProcessClass::Fitness)fitness), worstFit((ProcessClass::Fitness)fitness), count(count) {}

		RoleFitness() : bestFit(ProcessClass::NeverAssign), worstFit(ProcessClass::NeverAssign), count(0) {}

		RoleFitness(RoleFitness first, RoleFitness second) : bestFit(std::min(first.worstFit, second.worstFit)), worstFit(std::max(first.worstFit, second.worstFit)), count(first.count + second.count) {}

		RoleFitness( vector<std::pair<WorkerInterface, ProcessClass>> workers, ProcessClass::ClusterRole role ) {
			worstFit = ProcessClass::BestFit;
			bestFit = ProcessClass::NeverAssign;
			for(auto it : workers) {
				auto thisFit = it.second.machineClassFitness( role );
				worstFit = std::max(worstFit, thisFit);
				bestFit = std::min(bestFit, thisFit);
			}
			count = workers.size();
		}

		RoleFitness( std::vector<ProcessClass> classes, ProcessClass::ClusterRole role ) {
			worstFit = ProcessClass::BestFit;
			bestFit = ProcessClass::NeverAssign;
			for(auto it : classes) {
				auto thisFit = it.machineClassFitness( role );
				worstFit = std::max(worstFit, thisFit);
				bestFit = std::min(bestFit, thisFit);
			}
			count = classes.size();
		}

		bool operator < (RoleFitness const& r) const {
			if (worstFit != r.worstFit) return worstFit < r.worstFit;
			if (bestFit != r.bestFit) return bestFit < r.bestFit;
			return count > r.count;
		}

		bool betterFitness (RoleFitness const& r) const {
			if (worstFit != r.worstFit) return worstFit < r.worstFit;
			if (bestFit != r.bestFit) return bestFit < r.bestFit;
			return false;
		}

		bool betterCount (RoleFitness const& r) const {
			if(count > r.count) return true;
			return worstFit < r.worstFit;
		}

		bool operator == (RoleFitness const& r) const { return worstFit == r.worstFit && bestFit == r.bestFit && count == r.count; }

		std::string toString() const { return format("%d %d &d", bestFit, worstFit, count); }
	};

	std::set<Optional<Standalone<StringRef>>> getDatacenters( DatabaseConfiguration const& conf, bool checkStable = false ) {
		std::set<Optional<Standalone<StringRef>>> result;
		for( auto& it : id_worker )
			if( workerAvailable( it.second, checkStable ) && !conf.isExcludedServer( it.second.interf.address() ) )
				result.insert(it.second.interf.locality.dcId());
		return result;
	}

	RecruitRemoteFromConfigurationReply findRemoteWorkersForConfiguration( RecruitRemoteFromConfigurationRequest const& req ) {
		RecruitRemoteFromConfigurationReply result;
		std::map< Optional<Standalone<StringRef>>, int> id_used;

		id_used[masterProcessId]++;
		id_used[clusterControllerProcessId]++;

		std::set<Optional<Key>> remoteDC;
		remoteDC.insert(req.dcId);

		auto remoteLogs = getWorkersForTlogs( req.configuration, req.configuration.getRemoteTLogReplicationFactor(), req.configuration.getDesiredRemoteLogs(), req.configuration.getRemoteTLogPolicy(), id_used, false, remoteDC );
		for(int i = 0; i < remoteLogs.size(); i++) {
			result.remoteTLogs.push_back(remoteLogs[i].first);
		}

		auto logRouters = getWorkersForRoleInDatacenter( req.dcId, ProcessClass::LogRouter, req.logRouterCount, req.configuration, id_used );
		for(int i = 0; i < logRouters.size(); i++) {
			result.logRouters.push_back(logRouters[i].first);
		}

		if(!remoteStartTime.present()) {
			double maxAvailableTime = 0;
			for(auto& it : result.remoteTLogs) {
				maxAvailableTime = std::max(maxAvailableTime, id_worker[it.locality.processId()].lastAvailableTime);
			}
			for(auto& it : result.logRouters) {
				maxAvailableTime = std::max(maxAvailableTime, id_worker[it.locality.processId()].lastAvailableTime);
			}
			remoteStartTime = maxAvailableTime;
		}

		if( now() - remoteStartTime.get() < SERVER_KNOBS->WAIT_FOR_GOOD_REMOTE_RECRUITMENT_DELAY &&
			( ( RoleFitness(SERVER_KNOBS->EXPECTED_TLOG_FITNESS, req.configuration.getDesiredRemoteLogs()).betterCount(RoleFitness(remoteLogs, ProcessClass::TLog)) ) ||
			  ( RoleFitness(SERVER_KNOBS->EXPECTED_LOG_ROUTER_FITNESS, req.logRouterCount).betterCount(RoleFitness(logRouters, ProcessClass::LogRouter)) ) ) ) {
			throw operation_failed();
		}

		return result;
	}

	ErrorOr<RecruitFromConfigurationReply> findWorkersForConfiguration( RecruitFromConfigurationRequest const& req, Optional<Key> dcId ) {
		RecruitFromConfigurationReply result;
		std::map< Optional<Standalone<StringRef>>, int> id_used;
		id_used[masterProcessId]++;
		id_used[clusterControllerProcessId]++;

		ASSERT(dcId.present());

		std::set<Optional<Key>> primaryDC;
		primaryDC.insert(dcId);
		result.dcId = dcId;

		RegionInfo region;
		for(auto& r : req.configuration.regions) {
			if(r.dcId == dcId.get()) {
				region = r;
				break;
			}
		}

		if(req.recruitSeedServers) {
			auto primaryStorageServers = getWorkersForSeedServers( req.configuration, req.configuration.storagePolicy, dcId );
			for(int i = 0; i < primaryStorageServers.size(); i++) {
				result.storageServers.push_back(primaryStorageServers[i].first);
			}
		}

		auto tlogs = getWorkersForTlogs( req.configuration, req.configuration.tLogReplicationFactor, req.configuration.getDesiredLogs(), req.configuration.tLogPolicy, id_used, false, primaryDC );
		for(int i = 0; i < tlogs.size(); i++) {
			result.tLogs.push_back(tlogs[i].first);
		}

		std::vector<std::pair<WorkerInterface, ProcessClass>> satelliteLogs;
		if(region.satelliteTLogReplicationFactor > 0) {
			satelliteLogs = getWorkersForSatelliteLogs( req.configuration, region, id_used, result.satelliteFallback );
			for(int i = 0; i < satelliteLogs.size(); i++) {
				result.satelliteTLogs.push_back(satelliteLogs[i].first);
			}
		}

		auto first_resolver = getWorkerForRoleInDatacenter( dcId, ProcessClass::Resolver, ProcessClass::ExcludeFit, req.configuration, id_used );
		auto first_proxy = getWorkerForRoleInDatacenter( dcId, ProcessClass::Proxy, ProcessClass::ExcludeFit, req.configuration, id_used );

		auto proxies = getWorkersForRoleInDatacenter( dcId, ProcessClass::Proxy, req.configuration.getDesiredProxies()-1, req.configuration, id_used, first_proxy );
		auto resolvers = getWorkersForRoleInDatacenter( dcId, ProcessClass::Resolver, req.configuration.getDesiredResolvers()-1, req.configuration, id_used, first_resolver );

		proxies.push_back(first_proxy.worker);
		resolvers.push_back(first_resolver.worker);

		for(int i = 0; i < resolvers.size(); i++)
			result.resolvers.push_back(resolvers[i].first);
		for(int i = 0; i < proxies.size(); i++)
			result.proxies.push_back(proxies[i].first);

		auto oldLogRouters = getWorkersForRoleInDatacenter( dcId, ProcessClass::LogRouter, req.maxOldLogRouters, req.configuration, id_used );
		for(int i = 0; i < oldLogRouters.size(); i++) {
			result.oldLogRouters.push_back(oldLogRouters[i].first);
		}

		if( now() - startTime < SERVER_KNOBS->WAIT_FOR_GOOD_RECRUITMENT_DELAY &&
			( RoleFitness(SERVER_KNOBS->EXPECTED_TLOG_FITNESS, req.configuration.getDesiredLogs()).betterCount(RoleFitness(tlogs, ProcessClass::TLog)) ||
			  ( region.satelliteTLogReplicationFactor > 0 && RoleFitness(SERVER_KNOBS->EXPECTED_TLOG_FITNESS, req.configuration.getDesiredSatelliteLogs(dcId)).betterCount(RoleFitness(satelliteLogs, ProcessClass::TLog)) ) ||
			  RoleFitness(SERVER_KNOBS->EXPECTED_PROXY_FITNESS, req.configuration.getDesiredProxies()).betterCount(RoleFitness(proxies, ProcessClass::Proxy)) ||
			  RoleFitness(SERVER_KNOBS->EXPECTED_RESOLVER_FITNESS, req.configuration.getDesiredResolvers()).betterCount(RoleFitness(resolvers, ProcessClass::Resolver)) ) ) {
			return operation_failed();
		}

		return result;
	}

	RecruitFromConfigurationReply findWorkersForConfiguration( RecruitFromConfigurationRequest const& req ) {
		if(req.configuration.regions.size() > 1) {
			std::vector<RegionInfo> regions = req.configuration.regions;
			if(regions[0].priority == regions[1].priority && clusterControllerDcId.present() && regions[1].dcId == clusterControllerDcId.get()) {
				std::swap(regions[0], regions[1]);
			}

			if(clusterControllerDcId.present() && regions[1].dcId == clusterControllerDcId.get() && regions[1].priority >= 0 && (!versionDifferenceUpdated || datacenterVersionDifference >= SERVER_KNOBS->MAX_VERSION_DIFFERENCE)) {
				std::swap(regions[0], regions[1]);
			}

			bool setPrimaryDesired = false;
			try {
				auto reply = findWorkersForConfiguration(req, regions[0].dcId);
				setPrimaryDesired = true;
				vector<Optional<Key>> dcPriority;
				dcPriority.push_back(regions[0].dcId);
				dcPriority.push_back(regions[1].dcId);
				desiredDcIds.set(dcPriority);
				if(reply.isError()) {
					throw reply.getError();
				} else if(clusterControllerDcId.present() && regions[0].dcId == clusterControllerDcId.get()) {
					return reply.get();
				}
				throw no_more_servers();
			} catch( Error& e ) {
				if(now() - startTime < SERVER_KNOBS->WAIT_FOR_GOOD_REMOTE_RECRUITMENT_DELAY && (!clusterControllerDcId.present() || regions[1].dcId != clusterControllerDcId.get())) {
					throw operation_failed();
				}

				if (e.code() != error_code_no_more_servers || regions[1].priority < 0) {
					throw;
				}
				TraceEvent(SevWarn, "AttemptingRecruitmentInRemoteDC", id).error(e);
				auto reply = findWorkersForConfiguration(req, regions[1].dcId);
				if(!setPrimaryDesired) {
					vector<Optional<Key>> dcPriority;
					dcPriority.push_back(regions[1].dcId);
					dcPriority.push_back(regions[0].dcId);
					desiredDcIds.set(dcPriority);
				}
				if(reply.isError()) {
					throw reply.getError();
				} else if(clusterControllerDcId.present() && regions[1].dcId == clusterControllerDcId.get()) {
					return reply.get();
				}
				throw;
			}
		} else if(req.configuration.regions.size() == 1) {
			vector<Optional<Key>> dcPriority;
			dcPriority.push_back(req.configuration.regions[0].dcId);
			desiredDcIds.set(dcPriority);
			auto reply = findWorkersForConfiguration(req, req.configuration.regions[0].dcId);
			if(reply.isError()) {
				throw reply.getError();
			} else if(clusterControllerDcId.present() && req.configuration.regions[0].dcId == clusterControllerDcId.get()) {
				return reply.get();
			}
			throw no_more_servers();
		} else {
			RecruitFromConfigurationReply result;
			std::map< Optional<Standalone<StringRef>>, int> id_used;
			id_used[masterProcessId]++;
			id_used[clusterControllerProcessId]++;
			
			auto tlogs = getWorkersForTlogs( req.configuration, req.configuration.tLogReplicationFactor, req.configuration.getDesiredLogs(), req.configuration.tLogPolicy, id_used );
			for(int i = 0; i < tlogs.size(); i++) {
				result.tLogs.push_back(tlogs[i].first);
			}

			if(req.recruitSeedServers) {
				auto primaryStorageServers = getWorkersForSeedServers( req.configuration, req.configuration.storagePolicy );
				for(int i = 0; i < primaryStorageServers.size(); i++)
					result.storageServers.push_back(primaryStorageServers[i].first);
			}

			auto datacenters = getDatacenters( req.configuration );

			RoleFitness bestFitness;
			int numEquivalent = 1;
			Optional<Key> bestDC;

			for(auto dcId : datacenters ) {
				try {
					//SOMEDAY: recruitment in other DCs besides the clusterControllerDcID will not account for the processes used by the master and cluster controller properly.
					auto used = id_used;
					auto first_resolver = getWorkerForRoleInDatacenter( dcId, ProcessClass::Resolver, ProcessClass::ExcludeFit, req.configuration, used );
					auto first_proxy = getWorkerForRoleInDatacenter( dcId, ProcessClass::Proxy, ProcessClass::ExcludeFit, req.configuration, used );

					auto proxies = getWorkersForRoleInDatacenter( dcId, ProcessClass::Proxy, req.configuration.getDesiredProxies()-1, req.configuration, used, first_proxy );
					auto resolvers = getWorkersForRoleInDatacenter( dcId, ProcessClass::Resolver, req.configuration.getDesiredResolvers()-1, req.configuration, used, first_resolver );

					proxies.push_back(first_proxy.worker);
					resolvers.push_back(first_resolver.worker);

					auto fitness = RoleFitness( RoleFitness(proxies, ProcessClass::Proxy), RoleFitness(resolvers, ProcessClass::Resolver) );

					if(dcId == clusterControllerDcId) {
						bestFitness = fitness;
						bestDC = dcId;
						for(int i = 0; i < resolvers.size(); i++)
							result.resolvers.push_back(resolvers[i].first);
						for(int i = 0; i < proxies.size(); i++)
							result.proxies.push_back(proxies[i].first);

						auto oldLogRouters = getWorkersForRoleInDatacenter( dcId, ProcessClass::LogRouter, req.maxOldLogRouters, req.configuration, used );
						for(int i = 0; i < oldLogRouters.size(); i++) {
							result.oldLogRouters.push_back(oldLogRouters[i].first);
						}
						break;
					} else {
						if(fitness < bestFitness) {
							bestFitness = fitness;
							numEquivalent = 1;
							bestDC = dcId;
						} else if( fitness == bestFitness && g_random->random01() < 1.0/++numEquivalent ) {
							bestDC = dcId;
						}
					}
				} catch( Error &e ) {
					if(e.code() != error_code_no_more_servers) {
						throw;
					}
				}
			}

			if(bestDC != clusterControllerDcId) {
				vector<Optional<Key>> dcPriority;
				dcPriority.push_back(bestDC);
				desiredDcIds.set(dcPriority);
				throw no_more_servers();
			}
			//If this cluster controller dies, do not prioritize recruiting the next one in the same DC
			desiredDcIds.set(vector<Optional<Key>>());
			TraceEvent("FindWorkersForConfig").detail("Replication", req.configuration.tLogReplicationFactor)
				.detail("DesiredLogs", req.configuration.getDesiredLogs()).detail("ActualLogs", result.tLogs.size())
				.detail("DesiredProxies", req.configuration.getDesiredProxies()).detail("ActualProxies", result.proxies.size())
				.detail("DesiredResolvers", req.configuration.getDesiredResolvers()).detail("ActualResolvers", result.resolvers.size());

			if( now() - startTime < SERVER_KNOBS->WAIT_FOR_GOOD_RECRUITMENT_DELAY &&
				( RoleFitness(SERVER_KNOBS->EXPECTED_TLOG_FITNESS, req.configuration.getDesiredLogs()).betterCount(RoleFitness(tlogs, ProcessClass::TLog)) ||
				  RoleFitness(std::min(SERVER_KNOBS->EXPECTED_PROXY_FITNESS, SERVER_KNOBS->EXPECTED_RESOLVER_FITNESS), std::max(SERVER_KNOBS->EXPECTED_PROXY_FITNESS, SERVER_KNOBS->EXPECTED_RESOLVER_FITNESS), req.configuration.getDesiredProxies()+req.configuration.getDesiredResolvers()).betterCount(bestFitness) ) ) {
				throw operation_failed();
			}

			return result;
		}
	}

	void checkRegions(const std::vector<RegionInfo>& regions) {
		if(desiredDcIds.get().present() && desiredDcIds.get().get().size() == 2 && desiredDcIds.get().get()[0].get() == regions[0].dcId && desiredDcIds.get().get()[1].get() == regions[1].dcId) {
			return;
		}

		try {
			std::map< Optional<Standalone<StringRef>>, int> id_used;
			getWorkerForRoleInDatacenter(regions[0].dcId, ProcessClass::ClusterController, ProcessClass::ExcludeFit, db.config, id_used, true);
			getWorkerForRoleInDatacenter(regions[0].dcId, ProcessClass::Master, ProcessClass::ExcludeFit, db.config, id_used, true);

			std::set<Optional<Key>> primaryDC;
			primaryDC.insert(regions[0].dcId);
			getWorkersForTlogs(db.config, db.config.tLogReplicationFactor, db.config.getDesiredLogs(), db.config.tLogPolicy, id_used, true, primaryDC);
			if(regions[0].satelliteTLogReplicationFactor > 0) {
				bool satelliteFallback = false;
				getWorkersForSatelliteLogs(db.config, regions[0], id_used, satelliteFallback, true);
			}

			getWorkerForRoleInDatacenter( regions[0].dcId, ProcessClass::Resolver, ProcessClass::ExcludeFit, db.config, id_used, true );
			getWorkerForRoleInDatacenter( regions[0].dcId, ProcessClass::Proxy, ProcessClass::ExcludeFit, db.config, id_used, true );

			vector<Optional<Key>> dcPriority;
			dcPriority.push_back(regions[0].dcId);
			dcPriority.push_back(regions[1].dcId);
			desiredDcIds.set(dcPriority);
		} catch( Error &e ) {
			if(e.code() != error_code_no_more_servers) {
				throw;
			}
		}
	}

	void checkRecoveryStalled() {
		if( (db.serverInfo->get().recoveryState == RecoveryState::RECRUITING || db.serverInfo->get().recoveryState == RecoveryState::ACCEPTING_COMMITS || db.serverInfo->get().recoveryState == RecoveryState::ALL_LOGS_RECRUITED) && db.recoveryStalled ) {
			if(db.config.regions.size() > 1 && clusterControllerDcId.present()) {
				auto regions = db.config.regions;
				if(clusterControllerDcId.get() == regions[0].dcId) {
					std::swap(regions[0], regions[1]);
				}
				ASSERT(clusterControllerDcId.get() == regions[1].dcId);
				checkRegions(regions);
			}
		}
	}

	//FIXME: determine when to fail the cluster controller when a primaryDC has not been set
	bool betterMasterExists() {
		ServerDBInfo dbi = db.serverInfo->get();

		if(dbi.recoveryState < RecoveryState::ACCEPTING_COMMITS) {
			return false;
		}

		// Do not trigger better master exists if the cluster controller is excluded, since the master will change anyways once the cluster controller is moved
		if(id_worker[clusterControllerProcessId].priorityInfo.isExcluded) {
			return false;
		}

		if(db.config.regions.size() > 1 && clusterControllerDcId.present() && db.config.regions[0].priority > db.config.regions[1].priority &&
			db.config.regions[0].dcId != clusterControllerDcId.get() && versionDifferenceUpdated && datacenterVersionDifference < SERVER_KNOBS->MAX_VERSION_DIFFERENCE) {
			checkRegions(db.config.regions);
		}

		// Get master process
		auto masterWorker = id_worker.find(dbi.master.locality.processId());
		if(masterWorker == id_worker.end()) {
			return false;
		}

		// Get tlog processes
		std::vector<std::pair<WorkerInterface, ProcessClass>> tlogs;
		std::vector<std::pair<WorkerInterface, ProcessClass>> remote_tlogs;
		std::vector<std::pair<WorkerInterface, ProcessClass>> satellite_tlogs;
		std::vector<std::pair<WorkerInterface, ProcessClass>> log_routers;
		std::set<NetworkAddress> logRouterAddresses;

		for( auto& logSet : dbi.logSystemConfig.tLogs ) {
			for( auto& it : logSet.tLogs ) {
				auto tlogWorker = id_worker.find(it.interf().locality.processId());
				if ( tlogWorker == id_worker.end() )
					return false;
				if ( tlogWorker->second.priorityInfo.isExcluded )
					return true;

				if(logSet.isLocal && logSet.locality == tagLocalitySatellite) {
					satellite_tlogs.push_back(std::make_pair(tlogWorker->second.interf, tlogWorker->second.processClass));
				}
				else if(logSet.isLocal) {
					tlogs.push_back(std::make_pair(tlogWorker->second.interf, tlogWorker->second.processClass));
				} else {
					remote_tlogs.push_back(std::make_pair(tlogWorker->second.interf, tlogWorker->second.processClass));
				}
			}

			for( auto& it : logSet.logRouters ) {
				auto tlogWorker = id_worker.find(it.interf().locality.processId());
				if ( tlogWorker == id_worker.end() )
					return false;
				if ( tlogWorker->second.priorityInfo.isExcluded )
					return true;
				if( !logRouterAddresses.count( tlogWorker->second.interf.address() ) ) {
					logRouterAddresses.insert( tlogWorker->second.interf.address() );
					log_routers.push_back(std::make_pair(tlogWorker->second.interf, tlogWorker->second.processClass));
				}
			}
		}

		// Get proxy classes
		std::vector<ProcessClass> proxyClasses;
		for(auto& it : dbi.client.proxies ) {
			auto proxyWorker = id_worker.find(it.locality.processId());
			if ( proxyWorker == id_worker.end() )
				return false;
			if ( proxyWorker->second.priorityInfo.isExcluded )
				return true;
			proxyClasses.push_back(proxyWorker->second.processClass);
		}

		// Get resolver classes
		std::vector<ProcessClass> resolverClasses;
		for(auto& it : dbi.resolvers ) {
			auto resolverWorker = id_worker.find(it.locality.processId());
			if ( resolverWorker == id_worker.end() )
				return false;
			if ( resolverWorker->second.priorityInfo.isExcluded )
				return true;
			resolverClasses.push_back(resolverWorker->second.processClass);
		}

		// Check master fitness. Don't return false if master is excluded in case all the processes are excluded, we still need master for recovery.
		ProcessClass::Fitness oldMasterFit = masterWorker->second.processClass.machineClassFitness( ProcessClass::Master );
		if(db.config.isExcludedServer(dbi.master.address())) {
			oldMasterFit = std::max(oldMasterFit, ProcessClass::ExcludeFit);
		}

		std::map< Optional<Standalone<StringRef>>, int> id_used;
		id_used[clusterControllerProcessId]++;
		WorkerFitnessInfo mworker = getWorkerForRoleInDatacenter(clusterControllerDcId, ProcessClass::Master, ProcessClass::NeverAssign, db.config, id_used, true);

		if ( oldMasterFit < mworker.fitness )
			return false;
		if ( oldMasterFit > mworker.fitness || ( dbi.master.locality.processId() == clusterControllerProcessId && mworker.worker.first.locality.processId() != clusterControllerProcessId ) )
			return true;

		std::set<Optional<Key>> primaryDC;
		std::set<Optional<Key>> remoteDC;

		RegionInfo region;
		if(db.config.regions.size() && clusterControllerDcId.present()) {
			primaryDC.insert(clusterControllerDcId);
			for(auto& r : db.config.regions) {
				if(r.dcId != clusterControllerDcId.get()) {
					ASSERT(remoteDC.empty());
					remoteDC.insert(r.dcId);
				} else {
					ASSERT(region.dcId == StringRef());
					region = r;
				}
			}
		}

		// Check tLog fitness
		RoleFitness oldTLogFit(tlogs, ProcessClass::TLog);
		RoleFitness newTLogFit(getWorkersForTlogs(db.config, db.config.tLogReplicationFactor, db.config.getDesiredLogs(), db.config.tLogPolicy, id_used, true, primaryDC), ProcessClass::TLog);

		if(oldTLogFit < newTLogFit) return false;

		bool oldSatelliteFallback = false;
		for(auto& logSet : dbi.logSystemConfig.tLogs) {
			if(logSet.isLocal && logSet.locality == tagLocalitySatellite) {
				oldSatelliteFallback = logSet.tLogPolicy->info() != region.satelliteTLogPolicy->info();
				ASSERT(!oldSatelliteFallback || logSet.tLogPolicy->info() == region.satelliteTLogPolicyFallback->info());
				break;
			}
		}

		RoleFitness oldSatelliteTLogFit(satellite_tlogs, ProcessClass::TLog);
		bool newSatelliteFallback = false;
		RoleFitness newSatelliteTLogFit(region.satelliteTLogReplicationFactor > 0 ? getWorkersForSatelliteLogs(db.config, region, id_used, newSatelliteFallback, true) : satellite_tlogs, ProcessClass::TLog);

		if(oldSatelliteTLogFit < newSatelliteTLogFit)
			return false;
		if(!oldSatelliteFallback && newSatelliteFallback)
			return false;

		RoleFitness oldRemoteTLogFit(remote_tlogs, ProcessClass::TLog);
		RoleFitness newRemoteTLogFit((db.config.usableRegions > 1 && dbi.recoveryState == RecoveryState::FULLY_RECOVERED) ? getWorkersForTlogs(db.config, db.config.getRemoteTLogReplicationFactor(), db.config.getDesiredRemoteLogs(), db.config.getRemoteTLogPolicy(), id_used, true, remoteDC) : remote_tlogs, ProcessClass::TLog);

		if(oldRemoteTLogFit < newRemoteTLogFit) return false;

		int oldRouterCount = oldTLogFit.count * std::max<int>(1, db.config.desiredLogRouterCount / std::max(1,oldTLogFit.count));
		int newRouterCount = newTLogFit.count * std::max<int>(1, db.config.desiredLogRouterCount / std::max(1,newTLogFit.count));
		RoleFitness oldLogRoutersFit(log_routers, ProcessClass::LogRouter);
		RoleFitness newLogRoutersFit((db.config.usableRegions > 1 && dbi.recoveryState == RecoveryState::FULLY_RECOVERED) ? getWorkersForRoleInDatacenter( *remoteDC.begin(), ProcessClass::LogRouter, newRouterCount, db.config, id_used, Optional<WorkerFitnessInfo>(), true ) : log_routers, ProcessClass::LogRouter);

		if(oldLogRoutersFit.count < oldRouterCount) {
			oldLogRoutersFit.worstFit = ProcessClass::NeverAssign;
		}
		if(newLogRoutersFit.count < newRouterCount) {
			newLogRoutersFit.worstFit = ProcessClass::NeverAssign;
		}

		if(oldLogRoutersFit < newLogRoutersFit) return false;

		// Check proxy/resolver fitness
		RoleFitness oldInFit(RoleFitness(proxyClasses, ProcessClass::Proxy), RoleFitness(resolverClasses, ProcessClass::Resolver));

		auto first_resolver = getWorkerForRoleInDatacenter( clusterControllerDcId, ProcessClass::Resolver, ProcessClass::ExcludeFit, db.config, id_used, true );
		auto first_proxy = getWorkerForRoleInDatacenter( clusterControllerDcId, ProcessClass::Proxy, ProcessClass::ExcludeFit, db.config, id_used, true );

		auto proxies = getWorkersForRoleInDatacenter( clusterControllerDcId, ProcessClass::Proxy, db.config.getDesiredProxies()-1, db.config, id_used, first_proxy, true );
		auto resolvers = getWorkersForRoleInDatacenter( clusterControllerDcId, ProcessClass::Resolver, db.config.getDesiredResolvers()-1, db.config, id_used, first_resolver, true );
		proxies.push_back(first_proxy.worker);
		resolvers.push_back(first_resolver.worker);

		RoleFitness newInFit(RoleFitness(proxies, ProcessClass::Proxy), RoleFitness(resolvers, ProcessClass::Resolver));

		if(oldInFit.betterFitness(newInFit)) return false;

		if(oldTLogFit > newTLogFit || oldInFit > newInFit || (oldSatelliteFallback && !newSatelliteFallback) || oldSatelliteTLogFit > newSatelliteTLogFit || oldRemoteTLogFit > newRemoteTLogFit || oldLogRoutersFit > newLogRoutersFit) {
			TraceEvent("BetterMasterExists", id).detail("OldMasterFit", oldMasterFit).detail("NewMasterFit", mworker.fitness)
				.detail("OldTLogFit", oldTLogFit.toString()).detail("NewTLogFit", newTLogFit.toString())
				.detail("OldInFit", oldInFit.toString()).detail("NewInFit", newInFit.toString())
				.detail("OldSatelliteFit", oldSatelliteTLogFit.toString()).detail("NewSatelliteFit", newSatelliteTLogFit.toString())
				.detail("OldRemoteFit", oldRemoteTLogFit.toString()).detail("NewRemoteFit", newRemoteTLogFit.toString())
				.detail("OldRouterFit", oldLogRoutersFit.toString()).detail("NewRouterFit", newLogRoutersFit.toString())
				.detail("OldSatelliteFallback", oldSatelliteFallback).detail("NewSatelliteFallback", newSatelliteFallback);
			return true;
		}

		return false;
	}

	std::map< Optional<Standalone<StringRef>>, WorkerInfo > id_worker;
	std::map<  Optional<Standalone<StringRef>>, ProcessClass > id_class; //contains the mapping from process id to process class from the database
	Standalone<RangeResultRef> lastProcessClasses;
	bool gotProcessClasses;
	bool gotFullyRecoveredConfig;
	Optional<Standalone<StringRef>> masterProcessId;
	Optional<Standalone<StringRef>> clusterControllerProcessId;
	Optional<Standalone<StringRef>> clusterControllerDcId;
	AsyncVar<Optional<vector<Optional<Key>>>> desiredDcIds; //desired DC priorities
	AsyncVar<std::pair<bool,Optional<vector<Optional<Key>>>>> changingDcIds; //current DC priorities to change first, and whether that is the cluster controller
	AsyncVar<std::pair<bool,Optional<vector<Optional<Key>>>>> changedDcIds; //current DC priorities to change second, and whether the cluster controller has been changed
	UID id;
	std::vector<RecruitFromConfigurationRequest> outstandingRecruitmentRequests;
	std::vector<RecruitRemoteFromConfigurationRequest> outstandingRemoteRecruitmentRequests;
	std::vector<std::pair<RecruitStorageRequest, double>> outstandingStorageRequests;
	ActorCollection ac;
	UpdateWorkerList updateWorkerList;
	Future<Void> outstandingRequestChecker;

	DBInfo db;
	Database cx;
	double startTime;
	Optional<double> remoteStartTime;
	Version datacenterVersionDifference;
	bool versionDifferenceUpdated;

	ClusterControllerData( ClusterControllerFullInterface const& ccInterface, LocalityData const& locality )
		: id(ccInterface.id()), ac(false), outstandingRequestChecker(Void()), gotProcessClasses(false), gotFullyRecoveredConfig(false), startTime(now()), datacenterVersionDifference(0), versionDifferenceUpdated(false)
	{
		auto serverInfo = db.serverInfo->get();
		serverInfo.id = g_random->randomUniqueID();
		serverInfo.masterLifetime.ccID = id;
		serverInfo.clusterInterface = ccInterface;
		serverInfo.myLocality = locality;
		db.serverInfo->set( serverInfo );
		cx = openDBOnServer(db.serverInfo, TaskDefaultEndpoint, true, true);
	}

	~ClusterControllerData() {
		ac.clear(false);
		id_worker.clear();
	}
};

template <class K, class T>
vector<T> values( std::map<K,T> const& map ) {
	vector<T> t;
	for(auto i = map.begin(); i!=map.end(); ++i)
		t.push_back(i->second);
	return t;
}

ACTOR Future<Void> clusterWatchDatabase( ClusterControllerData* cluster, ClusterControllerData::DBInfo* db )
{
	state MasterInterface iMaster;

	// SOMEDAY: If there is already a non-failed master referenced by zkMasterInfo, use that one until it fails
	// When this someday is implemented, make sure forced failures still cause the master to be recruited again

	loop {
		TraceEvent("CCWDB", cluster->id);
		try {
			state double recoveryStart = now();
			TraceEvent("CCWDB", cluster->id).detail("Recruiting", "Master");

			while(!cluster->clusterControllerProcessId.present()) {
				wait( delay(SERVER_KNOBS->ATTEMPT_RECRUITMENT_DELAY) );
			}

			//We must recruit the master in the same data center as the cluster controller. 
			//This should always be possible, because we can recruit the master on the same process as the cluster controller.
			std::map< Optional<Standalone<StringRef>>, int> id_used;
			id_used[cluster->clusterControllerProcessId]++;
			state WorkerFitnessInfo masterWorker = cluster->getWorkerForRoleInDatacenter(cluster->clusterControllerDcId, ProcessClass::Master, ProcessClass::NeverAssign, db->config, id_used);
			if( ( masterWorker.worker.second.machineClassFitness( ProcessClass::Master ) > SERVER_KNOBS->EXPECTED_MASTER_FITNESS || masterWorker.worker.first.locality.processId() == cluster->clusterControllerProcessId )
				&& now() - cluster->startTime < SERVER_KNOBS->WAIT_FOR_GOOD_RECRUITMENT_DELAY ) {
				TraceEvent("CCWDB", cluster->id).detail("Fitness", masterWorker.worker.second.machineClassFitness( ProcessClass::Master ));
				wait( delay(SERVER_KNOBS->ATTEMPT_RECRUITMENT_DELAY) );
				continue;
			}
			RecruitMasterRequest rmq;
			rmq.lifetime = db->serverInfo->get().masterLifetime;
			rmq.forceRecovery = db->forceRecovery;

			cluster->masterProcessId = masterWorker.worker.first.locality.processId();
			cluster->db.unfinishedRecoveries++;
			ErrorOr<MasterInterface> newMaster = wait( masterWorker.worker.first.master.tryGetReply( rmq ) );
			if (newMaster.present()) {
				TraceEvent("CCWDB", cluster->id).detail("Recruited", newMaster.get().id());

				// for status tool
				TraceEvent("RecruitedMasterWorker", cluster->id)
					.detail("Address", newMaster.get().address())
					.trackLatest("RecruitedMasterWorker");

				iMaster = newMaster.get();

				db->masterRegistrationCount = 0;
				db->recoveryStalled = false;
				db->forceRecovery = false;
				db->forceMasterFailure = Promise<Void>();

				auto dbInfo = ServerDBInfo();
				dbInfo.master = iMaster;
				dbInfo.id = g_random->randomUniqueID();
				dbInfo.masterLifetime = db->serverInfo->get().masterLifetime;
				++dbInfo.masterLifetime;
				dbInfo.clusterInterface = db->serverInfo->get().clusterInterface;

				TraceEvent("CCWDB", cluster->id).detail("Lifetime", dbInfo.masterLifetime.toString()).detail("ChangeID", dbInfo.id);
				db->serverInfo->set( dbInfo );

				wait( delay(SERVER_KNOBS->MASTER_SPIN_DELAY) );  // Don't retry master recovery more than once per second, but don't delay the "first" recovery after more than a second of normal operation

				TraceEvent("CCWDB", cluster->id).detail("Watching", iMaster.id());

				// Master failure detection is pretty sensitive, but if we are in the middle of a very long recovery we really don't want to have to start over
				loop choose {
					when (wait( waitFailureClient( iMaster.waitFailure, db->masterRegistrationCount ?
						SERVER_KNOBS->MASTER_FAILURE_REACTION_TIME : (now() - recoveryStart) * SERVER_KNOBS->MASTER_FAILURE_SLOPE_DURING_RECOVERY,
						db->masterRegistrationCount ? -SERVER_KNOBS->MASTER_FAILURE_REACTION_TIME/SERVER_KNOBS->SECONDS_BEFORE_NO_FAILURE_DELAY : SERVER_KNOBS->MASTER_FAILURE_SLOPE_DURING_RECOVERY ) || db->forceMasterFailure.getFuture() )) { break; }
					when (wait( db->serverInfo->onChange() )) {}
				}

				TEST(true); // clusterWatchDatabase() master failed
				TraceEvent(SevWarn,"DetectedFailedMaster", cluster->id).detail("OldMaster", iMaster.id());
			} else {
				TEST(true); //clusterWatchDatabas() !newMaster.present()
				wait( delay(SERVER_KNOBS->MASTER_SPIN_DELAY) );
			}
		} catch (Error& e) {
			TraceEvent("CCWDB", cluster->id).error(e, true).detail("Master", iMaster.id());
			if (e.code() == error_code_actor_cancelled) throw;

			bool ok = e.code() == error_code_no_more_servers;
			TraceEvent(ok ? SevWarn : SevError,"ClusterWatchDatabaseRetrying", cluster->id).error(e);
			if (!ok)
				throw e;
			wait( delay(SERVER_KNOBS->ATTEMPT_RECRUITMENT_DELAY) );
		}
	}
}

void addIssue( ProcessIssuesMap& issueMap, NetworkAddress const& addr, std::string const& issue, UID& issueID ) {
	auto& e = issueMap[addr];
	e.first = issue;
	e.second = issueID = g_random->randomUniqueID();
	if (!issue.size()) issueMap.erase(addr);
}

void removeIssue( ProcessIssuesMap& issueMap, NetworkAddress const& addr, std::string const& issue, UID& issueID ) {
	if (!issue.size()) return;
	if ( issueMap.count(addr) && issueMap[addr].second == issueID )
		issueMap.erase( addr );
}

ACTOR Future<Void> clusterGetServerInfo(
	ClusterControllerData::DBInfo* db,
	UID knownServerInfoID,
	std::string issues,
	std::vector<NetworkAddress> incompatiblePeers,
	ReplyPromise<ServerDBInfo> reply)
{
	state UID issueID;
	addIssue( db->workersWithIssues, reply.getEndpoint().address[0], issues, issueID );
	for(auto it : incompatiblePeers) {
		db->incompatibleConnections[it] = now() + SERVER_KNOBS->INCOMPATIBLE_PEERS_LOGGING_INTERVAL;
	}

	while (db->serverInfo->get().id == knownServerInfoID) {
		choose {
			when (wait( db->serverInfo->onChange() )) {}
			when (wait( delayJittered( 300 ) )) { break; }  // The server might be long gone!
		}
	}

	removeIssue( db->workersWithIssues, reply.getEndpoint().address[0], issues, issueID );

	reply.send( db->serverInfo->get() );
	return Void();
}

ACTOR Future<Void> clusterOpenDatabase(
	ClusterControllerData::DBInfo* db,
	UID knownClientInfoID,
	std::string issues,
	Standalone<VectorRef<ClientVersionRef>> supportedVersions,
	Standalone<StringRef> traceLogGroup,
	ReplyPromise<ClientDBInfo> reply)
{
	// NOTE: The client no longer expects this function to return errors
	state UID issueID;
	addIssue( db->clientsWithIssues, reply.getEndpoint().address[0], issues, issueID );

	if(supportedVersions.size() > 0) {
		db->clientVersionMap[reply.getEndpoint().address[0]] = supportedVersions;
	}

	db->traceLogGroupMap[reply.getEndpoint().address[0]] = traceLogGroup.toString();

	while (db->clientInfo->get().id == knownClientInfoID) {
		choose {
			when (wait( db->clientInfo->onChange() )) {}
			when (wait( delayJittered( 300 ) )) { break; }  // The client might be long gone!
		}
	}

	removeIssue( db->clientsWithIssues, reply.getEndpoint().address[0], issues, issueID );
	db->clientVersionMap.erase(reply.getEndpoint().address[0]);
	db->traceLogGroupMap.erase(reply.getEndpoint().address[0]);

	reply.send( db->clientInfo->get() );
	return Void();
}

void checkOutstandingRecruitmentRequests( ClusterControllerData* self ) {
	for( int i = 0; i < self->outstandingRecruitmentRequests.size(); i++ ) {
		RecruitFromConfigurationRequest& req = self->outstandingRecruitmentRequests[i];
		try {
			req.reply.send( self->findWorkersForConfiguration( req ) );
			swapAndPop( &self->outstandingRecruitmentRequests, i-- );
		} catch (Error& e) {
			if (e.code() == error_code_no_more_servers || e.code() == error_code_operation_failed) {
				TraceEvent(SevWarn, "RecruitTLogMatchingSetNotAvailable", self->id).error(e);
			} else {
				TraceEvent(SevError, "RecruitTLogsRequestError", self->id).error(e);
				throw;
			}
		}
	}
}

void checkOutstandingRemoteRecruitmentRequests( ClusterControllerData* self ) {
	for( int i = 0; i < self->outstandingRemoteRecruitmentRequests.size(); i++ ) {
		RecruitRemoteFromConfigurationRequest& req = self->outstandingRemoteRecruitmentRequests[i];
		try {
			req.reply.send( self->findRemoteWorkersForConfiguration( req ) );
			swapAndPop( &self->outstandingRemoteRecruitmentRequests, i-- );
		} catch (Error& e) {
			if (e.code() == error_code_no_more_servers || e.code() == error_code_operation_failed) {
				TraceEvent(SevWarn, "RecruitRemoteTLogMatchingSetNotAvailable", self->id).error(e);
			} else {
				TraceEvent(SevError, "RecruitRemoteTLogsRequestError", self->id).error(e);
				throw;
			}
		}
	}
}

void checkOutstandingStorageRequests( ClusterControllerData* self ) {
	for( int i = 0; i < self->outstandingStorageRequests.size(); i++ ) {
		auto& req = self->outstandingStorageRequests[i];
		try {
			if(req.second < now()) {
				req.first.reply.sendError(timed_out());
				swapAndPop( &self->outstandingStorageRequests, i-- );
			} else {
				if(!self->gotProcessClasses && !req.first.criticalRecruitment)
					throw no_more_servers();

				auto worker = self->getStorageWorker(req.first);
				RecruitStorageReply rep;
				rep.worker = worker.first;
				rep.processClass = worker.second;
				req.first.reply.send( rep );
				swapAndPop( &self->outstandingStorageRequests, i-- );
			}
		} catch (Error& e) {
			if (e.code() == error_code_no_more_servers) {
				TraceEvent(SevWarn, "RecruitStorageNotAvailable", self->id).error(e);
			} else {
				TraceEvent(SevError, "RecruitStorageError", self->id).error(e);
				throw;
			}
		}
	}
}

ACTOR Future<Void> doCheckOutstandingRequests( ClusterControllerData* self ) {
	try {
		wait( delay(SERVER_KNOBS->CHECK_OUTSTANDING_INTERVAL) );

		checkOutstandingRecruitmentRequests( self );
		checkOutstandingRemoteRecruitmentRequests( self );
		checkOutstandingStorageRequests( self );

		self->checkRecoveryStalled();
		if (self->betterMasterExists()) {
			if (!self->db.forceMasterFailure.isSet()) {
				self->db.forceMasterFailure.send( Void() );
				TraceEvent("MasterRegistrationKill", self->id).detail("MasterId", self->db.serverInfo->get().master.id());
			}
		}
	} catch( Error &e ) {
		if(e.code() != error_code_operation_failed && e.code() != error_code_no_more_servers) {
			TraceEvent(SevError, "CheckOutstandingError").error(e);
		}
	}
	return Void();
}

void checkOutstandingRequests( ClusterControllerData* self ) {
	if( !self->outstandingRequestChecker.isReady() )
		return;

	self->outstandingRequestChecker = doCheckOutstandingRequests(self);
}

ACTOR Future<Void> rebootAndCheck( ClusterControllerData* cluster, Optional<Standalone<StringRef>> processID ) {
	auto watcher = cluster->id_worker.find(processID);
	ASSERT(watcher != cluster->id_worker.end());

	watcher->second.lastAvailableTime = now();
	watcher->second.reboots++;
	wait( delay( g_network->isSimulated() ? SERVER_KNOBS->SIM_SHUTDOWN_TIMEOUT : SERVER_KNOBS->SHUTDOWN_TIMEOUT ) );

	auto watcher = cluster->id_worker.find(processID);
	if(watcher != cluster->id_worker.end()) {
		watcher->second.reboots--;
		if( watcher->second.reboots < 2 )
			checkOutstandingRequests( cluster );
	}

	return Void();
}

ACTOR Future<Void> workerAvailabilityWatch( WorkerInterface worker, ProcessClass startingClass, ClusterControllerData* cluster ) {
	state Future<Void> failed = worker.address() == g_network->getLocalAddress() ? Never() : waitFailureClient( worker.waitFailure, SERVER_KNOBS->WORKER_FAILURE_TIME );
	cluster->updateWorkerList.set( worker.locality.processId(), ProcessData(worker.locality, startingClass, worker.address()) );
	loop {
		choose {
			when( wait( IFailureMonitor::failureMonitor().onStateEqual( worker.storage.getEndpoint(), FailureStatus(IFailureMonitor::failureMonitor().getState( worker.storage.getEndpoint() ).isAvailable()) ) ) ) {
				if( IFailureMonitor::failureMonitor().getState( worker.storage.getEndpoint() ).isAvailable() ) {
					cluster->ac.add( rebootAndCheck( cluster, worker.locality.processId() ) );
					checkOutstandingRequests( cluster );
				}
			}
			when( wait( failed ) ) {  // remove workers that have failed
				WorkerInfo& failedWorkerInfo = cluster->id_worker[ worker.locality.processId() ];
				if (!failedWorkerInfo.reply.isSet()) {
					failedWorkerInfo.reply.send( RegisterWorkerReply(failedWorkerInfo.processClass, failedWorkerInfo.priorityInfo) );
				}
				cluster->id_worker.erase( worker.locality.processId() );
				cluster->updateWorkerList.set( worker.locality.processId(), Optional<ProcessData>() );
				return Void();
			}
		}
	}
}

struct FailureStatusInfo {
	FailureStatus status;
	double lastRequestTime;
	double penultimateRequestTime;

	FailureStatusInfo() : lastRequestTime(0), penultimateRequestTime(0) {}

	void insertRequest(double now) {
		penultimateRequestTime = lastRequestTime;
		lastRequestTime = now;
	}

	double latency(double now) const {
		return std::max( now - lastRequestTime, lastRequestTime - penultimateRequestTime );
	}
};

//The failure monitor client relies on the fact that the failure detection server will not declare itself failed
ACTOR Future<Void> failureDetectionServer( UID uniqueID, ClusterControllerData::DBInfo* db, FutureStream< FailureMonitoringRequest > requests ) {
	state Version currentVersion = 0;
	state std::map<NetworkAddress, FailureStatusInfo> currentStatus;	// The status at currentVersion
	state std::deque<SystemFailureStatus> statusHistory;	// The last change in statusHistory is from currentVersion-1 to currentVersion
	state Future<Void> periodically = Void();
	state double lastT = 0;

	loop choose {
		when ( FailureMonitoringRequest req = waitNext( requests ) ) {
			if ( req.senderStatus.present() ) {
				// Update the status of requester, if necessary
				auto& address = req.reply.getEndpoint().address[0];
				auto& stat = currentStatus[ address ];
				auto& newStat = req.senderStatus.get();

				ASSERT( !newStat.failed || address != g_network->getLocalAddress() );

				stat.insertRequest(now());
				if (req.senderStatus != stat.status) {
					TraceEvent("FailureDetectionStatus", uniqueID).detail("System", address).detail("Status", newStat.failed ? "Failed" : "OK").detail("Why", "Request");
					statusHistory.push_back( SystemFailureStatus( address, newStat ) );
					++currentVersion;

					if (req.senderStatus == FailureStatus()){
						// failureMonitorClient reports explicitly that it is failed
						ASSERT(false); // This can't happen at the moment; if that changes, make this a TEST instead
						currentStatus.erase(address);
					} else {
						TEST(true);
						stat.status = newStat;
					}

					while (statusHistory.size() > currentStatus.size())
						statusHistory.pop_front();
				}
			}

			// Return delta-compressed status changes to requester
			Version reqVersion = req.failureInformationVersion;
			if (reqVersion > currentVersion){
				req.reply.sendError( future_version() );
				ASSERT(false);
			} else {
				TEST(true); // failureDetectionServer sending failure data to requester
				FailureMonitoringReply reply;
				reply.failureInformationVersion = currentVersion;
				if( req.senderStatus.present() ) {
					reply.clientRequestIntervalMS = FLOW_KNOBS->SERVER_REQUEST_INTERVAL * 1000;
					reply.considerServerFailedTimeoutMS = CLIENT_KNOBS->FAILURE_TIMEOUT_DELAY * 1000;
				} else {
					reply.clientRequestIntervalMS = FLOW_KNOBS->CLIENT_REQUEST_INTERVAL * 1000;
					reply.considerServerFailedTimeoutMS = CLIENT_KNOBS->CLIENT_FAILURE_TIMEOUT_DELAY * 1000;
				}

				ASSERT( currentVersion >= (int64_t)statusHistory.size());

				if (reqVersion < currentVersion - (int64_t)statusHistory.size() || reqVersion == 0) {
					// Send everything
					TEST(true); // failureDetectionServer sending all current data to requester
					reply.allOthersFailed = true;
					for(auto it = currentStatus.begin(); it != currentStatus.end(); ++it)
						reply.changes.push_back( reply.arena, SystemFailureStatus( it->first, it->second.status ) );
				} else {
					TEST(true); // failureDetectionServer sending delta-compressed data to requester
					// SOMEDAY: Send only the last change for a given address?
					reply.allOthersFailed = false;
					for(int v = reqVersion - currentVersion + statusHistory.size(); v < statusHistory.size(); v++) {
						reply.changes.push_back( reply.arena, statusHistory[v] );
					}
				}
				req.reply.send( reply );
			}
		}
		when ( wait( periodically ) ) {
			periodically = delay( FLOW_KNOBS->SERVER_REQUEST_INTERVAL );
			double t = now();
			if (lastT != 0 && t - lastT > 1)
				TraceEvent("LongDelayOnClusterController").detail("Duration", t - lastT);
			lastT = t;

			// Adapt to global unresponsiveness
			vector<double> delays;
			for(auto it=currentStatus.begin(); it!=currentStatus.end(); it++)
				if (it->second.penultimateRequestTime) {
					delays.push_back(it->second.latency(t));
					//TraceEvent("FDData", uniqueID).detail("S", it->first.toString()).detail("L", it->second.latency(t));
				}
			int pivot = std::max(0, (int)delays.size()-2);
			double pivotDelay = 0;
			if (delays.size()) {
				std::nth_element(delays.begin(), delays.begin()+pivot, delays.end());
				pivotDelay = *(delays.begin()+pivot);
			}
			pivotDelay = std::max(0.0, pivotDelay - FLOW_KNOBS->SERVER_REQUEST_INTERVAL);

			//TraceEvent("FailureDetectionPoll", uniqueID).detail("PivotDelay", pivotDelay).detail("Clients", currentStatus.size());
			//TraceEvent("FailureDetectionAcceptableDelay").detail("Delay", acceptableDelay1000);

			bool tooManyLogGenerations = std::max(db->unfinishedRecoveries, db->logGenerations) > CLIENT_KNOBS->FAILURE_MAX_GENERATIONS;

			for(auto it = currentStatus.begin(); it != currentStatus.end(); ) {
				double delay = t - it->second.lastRequestTime;
				if ( it->first != g_network->getLocalAddress() && ( tooManyLogGenerations ?
					( delay > CLIENT_KNOBS->FAILURE_EMERGENCY_DELAY ) :
					( delay > pivotDelay * 2 + FLOW_KNOBS->SERVER_REQUEST_INTERVAL + CLIENT_KNOBS->FAILURE_MIN_DELAY || delay > CLIENT_KNOBS->FAILURE_MAX_DELAY ) ) ) {
					//printf("Failure Detection Server: Status of '%s' is now '%s' after %f sec\n", it->first.toString().c_str(), "Failed", now() - it->second.lastRequestTime);
					TraceEvent("FailureDetectionStatus", uniqueID).detail("System", it->first).detail("Status","Failed").detail("Why", "Timeout").detail("LastRequestAge", delay)
						.detail("PivotDelay", pivotDelay).detail("UnfinishedRecoveries", db->unfinishedRecoveries).detail("LogGenerations", db->logGenerations);
					statusHistory.push_back( SystemFailureStatus( it->first, FailureStatus(true) ) );
					++currentVersion;
					it = currentStatus.erase(it);
					while (statusHistory.size() > currentStatus.size())
						statusHistory.pop_front();
				} else {
					++it;
				}
			}
		}
	}
}

ACTOR Future<vector<TLogInterface>> requireAll( vector<Future<Optional<vector<TLogInterface>>>> in ) {
	state vector<TLogInterface> out;
	state int i;
	for(i=0; i<in.size(); i++) {
		Optional<vector<TLogInterface>> x = wait(in[i]);
		if (!x.present()) throw recruitment_failed();
		out.insert(out.end(), x.get().begin(), x.get().end());
	}
	return out;
}

void clusterRecruitStorage( ClusterControllerData* self, RecruitStorageRequest req ) {
	try {
		if(!self->gotProcessClasses && !req.criticalRecruitment)
			throw no_more_servers();
		auto worker = self->getStorageWorker(req);
		RecruitStorageReply rep;
		rep.worker = worker.first;
		rep.processClass = worker.second;
		req.reply.send( rep );
	} catch ( Error& e ) {
		if (e.code() == error_code_no_more_servers) {
			self->outstandingStorageRequests.push_back( std::make_pair(req, now() + SERVER_KNOBS->RECRUITMENT_TIMEOUT) );
			TraceEvent(SevWarn, "RecruitStorageNotAvailable", self->id).error(e);
		} else {
			TraceEvent(SevError, "RecruitStorageError", self->id).error(e);
			throw;  // Any other error will bring down the cluster controller
		}
	}
}

ACTOR Future<Void> clusterRecruitFromConfiguration( ClusterControllerData* self, RecruitFromConfigurationRequest req ) {
	// At the moment this doesn't really need to be an actor (it always completes immediately)
	TEST(true); //ClusterController RecruitTLogsRequest
	loop {
		try {
			req.reply.send( self->findWorkersForConfiguration( req ) );
			return Void();
		} catch (Error& e) {
			if (e.code() == error_code_no_more_servers && now() - self->startTime >= SERVER_KNOBS->WAIT_FOR_GOOD_RECRUITMENT_DELAY) {
				self->outstandingRecruitmentRequests.push_back( req );
				TraceEvent(SevWarn, "RecruitFromConfigurationNotAvailable", self->id).error(e);
				return Void();
			} else if(e.code() == error_code_operation_failed || e.code() == error_code_no_more_servers) {
				//recruitment not good enough, try again
			}
			else {
				TraceEvent(SevError, "RecruitFromConfigurationError", self->id).error(e);
				throw;  // goodbye, cluster controller
			}
		}
		wait( delay(SERVER_KNOBS->ATTEMPT_RECRUITMENT_DELAY) );
	}
}

ACTOR Future<Void> clusterRecruitRemoteFromConfiguration( ClusterControllerData* self, RecruitRemoteFromConfigurationRequest req ) {
	// At the moment this doesn't really need to be an actor (it always completes immediately)
	TEST(true); //ClusterController RecruitTLogsRequest
	loop {
		try {
			req.reply.send( self->findRemoteWorkersForConfiguration( req ) );
			return Void();
		} catch (Error& e) {
			if (e.code() == error_code_no_more_servers && self->remoteStartTime.present() && now() - self->remoteStartTime.get() >= SERVER_KNOBS->WAIT_FOR_GOOD_REMOTE_RECRUITMENT_DELAY) {
				self->outstandingRemoteRecruitmentRequests.push_back( req );
				TraceEvent(SevWarn, "RecruitRemoteFromConfigurationNotAvailable", self->id).error(e);
				return Void();
			} else if(e.code() == error_code_operation_failed || e.code() == error_code_no_more_servers) {
				//recruitment not good enough, try again
			}
			else {
				TraceEvent(SevError, "RecruitRemoteFromConfigurationError", self->id).error(e);
				throw;  // goodbye, cluster controller
			}
		}
		wait( delay(SERVER_KNOBS->ATTEMPT_RECRUITMENT_DELAY) );
	}
}

void clusterRegisterMaster( ClusterControllerData* self, RegisterMasterRequest const& req ) {
	req.reply.send( Void() );

	TraceEvent("MasterRegistrationReceived", self->id).detail("MasterId", req.id).detail("Master", req.mi.toString()).detail("Tlogs", describe(req.logSystemConfig.tLogs)).detail("Resolvers", req.resolvers.size())
		.detail("RecoveryState", (int)req.recoveryState).detail("RegistrationCount", req.registrationCount).detail("Proxies", req.proxies.size()).detail("RecoveryCount", req.recoveryCount).detail("Stalled", req.recoveryStalled);

	//make sure the request comes from an active database
	auto db = &self->db;
	if ( db->serverInfo->get().master.id() != req.id || req.registrationCount <= db->masterRegistrationCount ) {
		TraceEvent("MasterRegistrationNotFound", self->id).detail("MasterId", req.id).detail("ExistingId", db->serverInfo->get().master.id()).detail("RegCount", req.registrationCount).detail("ExistingRegCount", db->masterRegistrationCount);
		return;
	}

	if ( req.recoveryState == RecoveryState::FULLY_RECOVERED ) {
		self->db.unfinishedRecoveries = 0;
		self->db.logGenerations = 0;
		ASSERT( !req.logSystemConfig.oldTLogs.size() );
	} else {
		self->db.logGenerations = std::max<int>(self->db.logGenerations, req.logSystemConfig.oldTLogs.size());
	}

	db->masterRegistrationCount = req.registrationCount;
	db->recoveryStalled = req.recoveryStalled;
	if ( req.configuration.present() ) {
		db->config = req.configuration.get();

		if ( req.recoveryState >= RecoveryState::ACCEPTING_COMMITS ) {
			self->gotFullyRecoveredConfig = true;
			db->fullyRecoveredConfig = req.configuration.get();
			for ( auto& it : self->id_worker ) {
				bool isExcludedFromConfig = db->fullyRecoveredConfig.isExcludedServer(it.second.interf.address());
				if ( it.second.priorityInfo.isExcluded != isExcludedFromConfig ) {
					it.second.priorityInfo.isExcluded = isExcludedFromConfig;
					if( !it.second.reply.isSet() ) {
						it.second.reply.send( RegisterWorkerReply( it.second.processClass, it.second.priorityInfo ) );
					}
				}
			}
		}
	}

	bool isChanged = false;
	auto dbInfo = self->db.serverInfo->get();

	if (dbInfo.recoveryState != req.recoveryState) {
		dbInfo.recoveryState = req.recoveryState;
		isChanged = true;
	}

	if (dbInfo.priorCommittedLogServers != req.priorCommittedLogServers) {
		dbInfo.priorCommittedLogServers = req.priorCommittedLogServers;
		isChanged = true;
	}

	// Construct the client information
	if (db->clientInfo->get().proxies != req.proxies) {
		isChanged = true;
		ClientDBInfo clientInfo;
		clientInfo.id = g_random->randomUniqueID();
		clientInfo.proxies = req.proxies;
		clientInfo.clientTxnInfoSampleRate = db->clientInfo->get().clientTxnInfoSampleRate;
		clientInfo.clientTxnInfoSizeLimit = db->clientInfo->get().clientTxnInfoSizeLimit;
		db->clientInfo->set( clientInfo );
		dbInfo.client = db->clientInfo->get();
	}

	if( !dbInfo.logSystemConfig.isEqual(req.logSystemConfig) ) {
		isChanged = true;
		dbInfo.logSystemConfig = req.logSystemConfig;
	}

	if( dbInfo.resolvers != req.resolvers ) {
		isChanged = true;
		dbInfo.resolvers = req.resolvers;
	}

	if( dbInfo.recoveryCount != req.recoveryCount ) {
		isChanged = true;
		dbInfo.recoveryCount = req.recoveryCount;
	}

	if( isChanged ) {
		dbInfo.id = g_random->randomUniqueID();
		self->db.serverInfo->set( dbInfo );
	}

	checkOutstandingRequests(self);
}

void registerWorker( RegisterWorkerRequest req, ClusterControllerData *self ) {
	WorkerInterface w = req.wi;
	ProcessClass newProcessClass = req.processClass;
	auto info = self->id_worker.find( w.locality.processId() );
	ClusterControllerPriorityInfo newPriorityInfo = req.priorityInfo;

	if(info == self->id_worker.end()) {
		TraceEvent("ClusterControllerActualWorkers", self->id).detail("WorkerId",w.id()).detailext("ProcessId", w.locality.processId()).detailext("ZoneId", w.locality.zoneId()).detailext("DataHall", w.locality.dataHallId()).detail("PClass", req.processClass.toString()).detail("Workers", self->id_worker.size());
	} else {
		TraceEvent("ClusterControllerWorkerAlreadyRegistered", self->id).suppressFor(1.0).detail("WorkerId",w.id()).detailext("ProcessId", w.locality.processId()).detailext("ZoneId", w.locality.zoneId()).detailext("DataHall", w.locality.dataHallId()).detail("PClass", req.processClass.toString()).detail("Workers", self->id_worker.size());
	}
	if ( w.address() == g_network->getLocalAddress() ) {
		self->clusterControllerProcessId = w.locality.processId();
		self->clusterControllerDcId = w.locality.dcId();
		if(self->changingDcIds.get().first) {
			if(self->changingDcIds.get().second.present()) {
				newPriorityInfo.dcFitness = ClusterControllerPriorityInfo::calculateDCFitness( w.locality.dcId(), self->changingDcIds.get().second.get() );
			}
		} else if(self->changedDcIds.get().second.present()) {
			newPriorityInfo.dcFitness = ClusterControllerPriorityInfo::calculateDCFitness( w.locality.dcId(), self->changedDcIds.get().second.get() );
		}
	} else {
		if(!self->changingDcIds.get().first) {
			if(self->changingDcIds.get().second.present()) {
				newPriorityInfo.dcFitness = ClusterControllerPriorityInfo::calculateDCFitness( w.locality.dcId(), self->changingDcIds.get().second.get() );
			}
		} else if(self->changedDcIds.get().second.present()) {
			newPriorityInfo.dcFitness = ClusterControllerPriorityInfo::calculateDCFitness( w.locality.dcId(), self->changedDcIds.get().second.get() );
		}
	}

	// Check process class and exclusive property
	if ( info == self->id_worker.end() || info->second.interf.id() != w.id() || req.generation >= info->second.gen ) {
		if ( self->gotProcessClasses ) {
			auto classIter = self->id_class.find(w.locality.processId());
			
			if( classIter != self->id_class.end() && (classIter->second.classSource() == ProcessClass::DBSource || req.initialClass.classType() == ProcessClass::UnsetClass)) {
				newProcessClass = classIter->second;
			} else {
				newProcessClass = req.initialClass;
			}
			newPriorityInfo.processClassFitness = newProcessClass.machineClassFitness(ProcessClass::ClusterController);
		}

		if ( self->gotFullyRecoveredConfig ) {
			newPriorityInfo.isExcluded = self->db.fullyRecoveredConfig.isExcludedServer(w.address());
		}

		// Notify the worker to register again with new process class/exclusive property
		if ( !req.reply.isSet() && newPriorityInfo != req.priorityInfo ) {
			req.reply.send( RegisterWorkerReply(newProcessClass, newPriorityInfo) );
		}
	}

	if( info == self->id_worker.end() ) {
		self->id_worker[w.locality.processId()] = WorkerInfo( workerAvailabilityWatch( w, newProcessClass, self ), req.reply, req.generation, w, req.initialClass, newProcessClass, newPriorityInfo );
		checkOutstandingRequests( self );
		return;
	}

	if( info->second.interf.id() != w.id() || req.generation >= info->second.gen ) {
		if (!info->second.reply.isSet()) {
			info->second.reply.send( Never() );
		}
		info->second.reply = req.reply;
		info->second.processClass = newProcessClass;
		info->second.priorityInfo = newPriorityInfo;
		info->second.initialClass = req.initialClass;
		info->second.gen = req.generation;

		if(info->second.interf.id() != w.id()) {
			info->second.interf = w;
			info->second.watcher = workerAvailabilityWatch( w, newProcessClass, self );
		}
		checkOutstandingRequests( self );
		return;
	}

	TEST(true); // Received an old worker registration request.
}

#define TIME_KEEPER_VERSION LiteralStringRef("1")

ACTOR Future<Void> timeKeeperSetVersion(ClusterControllerData *self) {
	state Reference<ReadYourWritesTransaction> tr = Reference<ReadYourWritesTransaction>(new ReadYourWritesTransaction(self->cx));
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);
			tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			tr->set(timeKeeperVersionKey, TIME_KEEPER_VERSION);
			wait(tr->commit());
			break;
		} catch (Error &e) {
			wait(tr->onError(e));
		}
	}

	return Void();
}

// This actor periodically gets read version and writes it to cluster with current timestamp as key. To avoid running
// out of space, it limits the max number of entries and clears old entries on each update. This mapping is used from
// backup and restore to get the version information for a timestamp.
ACTOR Future<Void> timeKeeper(ClusterControllerData *self) {
	state KeyBackedMap<int64_t, Version> versionMap(timeKeeperPrefixRange.begin);

	TraceEvent("TimeKeeperStarted");

	wait(timeKeeperSetVersion(self));

	loop {
		state Reference<ReadYourWritesTransaction> tr = Reference<ReadYourWritesTransaction>(new ReadYourWritesTransaction(self->cx));
		loop {
			try {
				if(!g_network->isSimulated()) {
					// This is done to provide an arbitrary logged transaction every ~10s.
					// FIXME: replace or augment this with logging on the proxy which tracks
					//       how long it is taking to hear responses from each other component.

					UID debugID = g_random->randomUniqueID();
					TraceEvent("TimeKeeperCommit", debugID);
					tr->debugTransaction(debugID);
				}
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr->setOption(FDBTransactionOptions::LOCK_AWARE);
				tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

				Optional<Value> disableValue = wait( tr->get(timeKeeperDisableKey) );
				if(disableValue.present()) {
					break;
				}

				Version v = tr->getReadVersion().get();
				int64_t currentTime = (int64_t)now();
				versionMap.set(tr, currentTime, v);

				int64_t ttl = currentTime - SERVER_KNOBS->TIME_KEEPER_DELAY * SERVER_KNOBS->TIME_KEEPER_MAX_ENTRIES;
				if (ttl > 0) {
					versionMap.erase(tr, 0, ttl);
				}

				wait(tr->commit());
				break;
			} catch (Error &e) {
				wait(tr->onError(e));
			}
		}

		wait(delay(SERVER_KNOBS->TIME_KEEPER_DELAY));
	}
}

ACTOR Future<Void> statusServer(FutureStream< StatusRequest> requests,
								ClusterControllerData *self,
								ServerCoordinators coordinators)
{
	// Seconds since the END of the last GetStatus executed
	state double last_request_time = 0.0;

	// Place to accumulate a batch of requests to respond to
	state std::vector<StatusRequest> requests_batch;

	loop {
		try {
			// Wait til first request is ready
			StatusRequest req = waitNext(requests);
			requests_batch.push_back(req);

			// Earliest time at which we may begin a new request
			double next_allowed_request_time = last_request_time + SERVER_KNOBS->STATUS_MIN_TIME_BETWEEN_REQUESTS;

			// Wait if needed to satisfy min_time knob, also allows more requets to queue up.
			double minwait = std::max(next_allowed_request_time - now(), 0.0);
			wait(delay(minwait));

			// Get all requests that are ready right *now*, before GetStatus() begins.
			// All of these requests will be responded to with the next GetStatus() result.
			while (requests.isReady())
				requests_batch.push_back(requests.pop());

			// Get status but trap errors to send back to client.
			vector<std::pair<WorkerInterface, ProcessClass>> workers;
			for(auto& it : self->id_worker)
				workers.push_back(std::make_pair(it.second.interf, it.second.processClass));

			std::vector<NetworkAddress> incompatibleConnections;
			for(auto it = self->db.incompatibleConnections.begin(); it != self->db.incompatibleConnections.end();) {
				if(it->second < now()) {
					it = self->db.incompatibleConnections.erase(it);
				} else {
					incompatibleConnections.push_back(it->first);
					it++;
				}
			}

			state ErrorOr<StatusReply> result = wait(errorOr(clusterGetStatus(self->db.serverInfo, self->cx, workers, self->db.workersWithIssues, self->db.clientsWithIssues, self->db.clientVersionMap, self->db.traceLogGroupMap, coordinators, incompatibleConnections, self->datacenterVersionDifference)));
			if (result.isError() && result.getError().code() == error_code_actor_cancelled)
				throw result.getError();

			// Update last_request_time now because GetStatus is finished and the delay is to be measured between requests
			last_request_time = now();

			while (!requests_batch.empty())
			{
				if (result.isError())
					requests_batch.back().reply.sendError(result.getError());
				else
					requests_batch.back().reply.send(result.get());
				requests_batch.pop_back();
				wait( yield() );
			}
		}
		catch (Error &e) {
			TraceEvent(SevError, "StatusServerError").error(e);
			throw e;
		}
	}
}

ACTOR Future<Void> monitorProcessClasses(ClusterControllerData *self) {

	state ReadYourWritesTransaction trVer( self->db.db );
	loop {
		try {
			trVer.setOption( FDBTransactionOptions::ACCESS_SYSTEM_KEYS );
			trVer.setOption( FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE );

			Optional<Value> val = wait(trVer.get(processClassVersionKey));

			if (val.present())
				break;

			Standalone<RangeResultRef> processClasses = wait( trVer.getRange( processClassKeys, CLIENT_KNOBS->TOO_MANY ) );
			ASSERT( !processClasses.more && processClasses.size() < CLIENT_KNOBS->TOO_MANY );

			trVer.clear(processClassKeys);
			trVer.set(processClassVersionKey, processClassVersionValue);
			for (auto it : processClasses) {
				UID processUid = decodeProcessClassKeyOld(it.key);
				trVer.set(processClassKeyFor(processUid.toString()), it.value);
			}

			wait(trVer.commit());
			TraceEvent("ProcessClassUpgrade");
			break;
		}
		catch(Error &e) {
			wait( trVer.onError(e) );
		}
	}

	loop {
		state ReadYourWritesTransaction tr( self->db.db );

		loop {
			try {
				tr.setOption( FDBTransactionOptions::ACCESS_SYSTEM_KEYS );
				tr.setOption( FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE );
				Standalone<RangeResultRef> processClasses = wait( tr.getRange( processClassKeys, CLIENT_KNOBS->TOO_MANY ) );
				ASSERT( !processClasses.more && processClasses.size() < CLIENT_KNOBS->TOO_MANY );

				if(processClasses != self->lastProcessClasses || !self->gotProcessClasses) {
					self->id_class.clear();
					for( int i = 0; i < processClasses.size(); i++ ) {
						auto c = decodeProcessClassValue( processClasses[i].value );
						ASSERT( c.classSource() != ProcessClass::CommandLineSource );
						self->id_class[decodeProcessClassKey( processClasses[i].key )] = c;
					}

					for( auto& w : self->id_worker ) {
						auto classIter = self->id_class.find(w.first);
						ProcessClass newProcessClass;

						if( classIter != self->id_class.end() && (classIter->second.classSource() == ProcessClass::DBSource || w.second.initialClass.classType() == ProcessClass::UnsetClass) ) {
							newProcessClass = classIter->second;
						} else {
							newProcessClass = w.second.initialClass;
						}

						
						if (newProcessClass != w.second.processClass) {
							w.second.processClass = newProcessClass;
							w.second.priorityInfo.processClassFitness = newProcessClass.machineClassFitness(ProcessClass::ClusterController);
							if (!w.second.reply.isSet()) {
								w.second.reply.send( RegisterWorkerReply(w.second.processClass, w.second.priorityInfo) );
							}
						}
					}

					self->lastProcessClasses = processClasses;
					self->gotProcessClasses = true;
					checkOutstandingRequests( self );
				}

				state Future<Void> watchFuture = tr.watch(processClassChangeKey);
				wait(tr.commit());
				wait(watchFuture);
				break;
			}
			catch(Error &e) {
				wait( tr.onError(e) );
			}
		}
	}
}

ACTOR Future<Void> monitorClientTxnInfoConfigs(ClusterControllerData::DBInfo* db) {
	loop {
		state ReadYourWritesTransaction tr(db->db);
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				state Optional<Value> rateVal = wait(tr.get(fdbClientInfoTxnSampleRate));
				state Optional<Value> limitVal = wait(tr.get(fdbClientInfoTxnSizeLimit));
				ClientDBInfo clientInfo = db->clientInfo->get();
				double sampleRate = rateVal.present() ? BinaryReader::fromStringRef<double>(rateVal.get(), Unversioned()) : std::numeric_limits<double>::infinity();
				int64_t sizeLimit = limitVal.present() ? BinaryReader::fromStringRef<int64_t>(limitVal.get(), Unversioned()) : -1;
				if (sampleRate != clientInfo.clientTxnInfoSampleRate || sizeLimit != clientInfo.clientTxnInfoSampleRate) {
					clientInfo.id = g_random->randomUniqueID();
					clientInfo.clientTxnInfoSampleRate = sampleRate;
					clientInfo.clientTxnInfoSizeLimit = sizeLimit;
					db->clientInfo->set(clientInfo);
				}
				
				state Future<Void> watchRateFuture = tr.watch(fdbClientInfoTxnSampleRate);
				state Future<Void> watchLimitFuture = tr.watch(fdbClientInfoTxnSizeLimit);
				wait(tr.commit());
				choose {
					when(wait(watchRateFuture)) { break; }
					when (wait(watchLimitFuture)) { break; }
				}
			}
			catch (Error &e) {
				wait(tr.onError(e));
			}
		}
	}
}

ACTOR Future<Void> updatedChangingDatacenters(ClusterControllerData *self) {
	//do not change the cluster controller until all the processes have had a chance to register
	wait( delay(SERVER_KNOBS->WAIT_FOR_GOOD_RECRUITMENT_DELAY) );
	loop {
		state Future<Void> onChange = self->desiredDcIds.onChange();
		if(!self->desiredDcIds.get().present()) {
			self->changingDcIds.set(std::make_pair(false,self->desiredDcIds.get()));
		} else {
			auto& worker = self->id_worker[self->clusterControllerProcessId];
			uint8_t newFitness = ClusterControllerPriorityInfo::calculateDCFitness( worker.interf.locality.dcId(), self->desiredDcIds.get().get() );
			self->changingDcIds.set(std::make_pair(worker.priorityInfo.dcFitness > newFitness,self->desiredDcIds.get()));

			TraceEvent("UpdateChangingDatacenter", self->id).detail("OldFitness", worker.priorityInfo.dcFitness).detail("NewFitness", newFitness);
			if ( worker.priorityInfo.dcFitness > newFitness ) {
				worker.priorityInfo.dcFitness = newFitness;
				if(!worker.reply.isSet()) {
					worker.reply.send( RegisterWorkerReply( worker.processClass, worker.priorityInfo ) );
				}
			} else {
				state int currentFit = ProcessClass::BestFit;
				while(currentFit <= ProcessClass::NeverAssign) {
					bool updated = false;
					for ( auto& it : self->id_worker ) {
						if( ( !it.second.priorityInfo.isExcluded && it.second.priorityInfo.processClassFitness == currentFit ) || currentFit == ProcessClass::NeverAssign ) {
							uint8_t fitness = ClusterControllerPriorityInfo::calculateDCFitness( it.second.interf.locality.dcId(), self->changingDcIds.get().second.get() );
							if ( it.first != self->clusterControllerProcessId && it.second.priorityInfo.dcFitness != fitness ) {
								updated = true;
								it.second.priorityInfo.dcFitness = fitness;
								if(!it.second.reply.isSet()) {
									it.second.reply.send( RegisterWorkerReply( it.second.processClass, it.second.priorityInfo ) );
								}
							}
						}
					}
					if(updated && currentFit < ProcessClass::NeverAssign) {
						wait( delay(SERVER_KNOBS->CC_CLASS_DELAY) );
					}
					currentFit++;
				}
			}
		}

		wait(onChange);
	}
}

ACTOR Future<Void> updatedChangedDatacenters(ClusterControllerData *self) {
	state Future<Void> changeDelay = delay(SERVER_KNOBS->CC_CHANGE_DELAY);
	state Future<Void> onChange = self->changingDcIds.onChange();
	loop {
		choose {
			when( wait(onChange) ) {
				changeDelay = delay(SERVER_KNOBS->CC_CHANGE_DELAY);
				onChange = self->changingDcIds.onChange();
			}
			when( wait(changeDelay) ) {
				changeDelay = Never();
				onChange = self->changingDcIds.onChange();

				self->changedDcIds.set(self->changingDcIds.get());
				if(self->changedDcIds.get().second.present()) {
					TraceEvent("UpdateChangedDatacenter", self->id).detail("CCFirst", self->changedDcIds.get().first);
					if( !self->changedDcIds.get().first ) {
						auto& worker = self->id_worker[self->clusterControllerProcessId];
						uint8_t newFitness = ClusterControllerPriorityInfo::calculateDCFitness( worker.interf.locality.dcId(), self->changedDcIds.get().second.get() );
						if( worker.priorityInfo.dcFitness != newFitness ) {
							worker.priorityInfo.dcFitness = newFitness;
							if(!worker.reply.isSet()) {
								worker.reply.send( RegisterWorkerReply( worker.processClass, worker.priorityInfo ) );
							}
						}
					} else {
						state int currentFit = ProcessClass::BestFit;
						while(currentFit <= ProcessClass::NeverAssign) {
							bool updated = false;
							for ( auto& it : self->id_worker ) {
								if( ( !it.second.priorityInfo.isExcluded && it.second.priorityInfo.processClassFitness == currentFit ) || currentFit == ProcessClass::NeverAssign ) {
									uint8_t fitness = ClusterControllerPriorityInfo::calculateDCFitness( it.second.interf.locality.dcId(), self->changedDcIds.get().second.get() );
									if ( it.first != self->clusterControllerProcessId && it.second.priorityInfo.dcFitness != fitness ) {
										updated = true;
										it.second.priorityInfo.dcFitness = fitness;
										if(!it.second.reply.isSet()) {
											it.second.reply.send( RegisterWorkerReply( it.second.processClass, it.second.priorityInfo ) );
										}
									}
								}
							}
							if(updated && currentFit < ProcessClass::NeverAssign) {
								wait( delay(SERVER_KNOBS->CC_CLASS_DELAY) );
							}
							currentFit++;
						}
					}
				}
			}
		}
	}
}

ACTOR Future<Void> updateDatacenterVersionDifference( ClusterControllerData *self ) {
	state double lastLogTime = 0;
	loop {
		self->versionDifferenceUpdated = false;
		if(self->db.serverInfo->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS && self->db.config.usableRegions == 1) {
			bool oldDifferenceTooLarge = !self->versionDifferenceUpdated || self->datacenterVersionDifference >= SERVER_KNOBS->MAX_VERSION_DIFFERENCE;
			self->versionDifferenceUpdated = true;
			self->datacenterVersionDifference = 0;

			if(oldDifferenceTooLarge) {
				checkOutstandingRequests(self);
			}

			wait(self->db.serverInfo->onChange());
			continue;
		}

		state Optional<TLogInterface> primaryLog;
		state Optional<TLogInterface> remoteLog;
		if(self->db.serverInfo->get().recoveryState >= RecoveryState::ALL_LOGS_RECRUITED) {
			for(auto& logSet : self->db.serverInfo->get().logSystemConfig.tLogs) {
				if(logSet.isLocal && logSet.locality != tagLocalitySatellite) {
					for(auto& tLog : logSet.tLogs) {
						if(tLog.present()) {
							primaryLog = tLog.interf();
							break;
						}
					}
				}
				if(!logSet.isLocal) {
					for(auto& tLog : logSet.tLogs) {
						if(tLog.present()) {
							remoteLog = tLog.interf();
							break;
						}
					}
				}
			}
		}

		if(!primaryLog.present() || !remoteLog.present()) {
			wait(self->db.serverInfo->onChange());
			continue;
		}

		state Future<Void> onChange = self->db.serverInfo->onChange();
		loop {
			state Future<TLogQueuingMetricsReply> primaryMetrics = brokenPromiseToNever( primaryLog.get().getQueuingMetrics.getReply( TLogQueuingMetricsRequest() ) );
			state Future<TLogQueuingMetricsReply> remoteMetrics = brokenPromiseToNever( remoteLog.get().getQueuingMetrics.getReply( TLogQueuingMetricsRequest() ) );

			wait( ( success(primaryMetrics) && success(remoteMetrics) ) || onChange );
			if(onChange.isReady()) {
				break;
			}

			bool oldDifferenceTooLarge = !self->versionDifferenceUpdated || self->datacenterVersionDifference >= SERVER_KNOBS->MAX_VERSION_DIFFERENCE;
			self->versionDifferenceUpdated = true;
			self->datacenterVersionDifference = primaryMetrics.get().v - remoteMetrics.get().v;

			if(oldDifferenceTooLarge && self->datacenterVersionDifference < SERVER_KNOBS->MAX_VERSION_DIFFERENCE) {
				checkOutstandingRequests(self);
			}

			if(now() - lastLogTime > SERVER_KNOBS->CLUSTER_CONTROLLER_LOGGING_DELAY) {
				lastLogTime = now();
				TraceEvent("DatacenterVersionDifference", self->id).detail("Difference", self->datacenterVersionDifference);
			}

			wait( delay(SERVER_KNOBS->VERSION_LAG_METRIC_INTERVAL) || onChange );
			if(onChange.isReady()) {
				break;
			}
		}
	}
}

ACTOR Future<Void> clusterControllerCore( ClusterControllerFullInterface interf, Future<Void> leaderFail, ServerCoordinators coordinators, LocalityData locality ) {
	state ClusterControllerData self( interf, locality );
	state Future<Void> coordinationPingDelay = delay( SERVER_KNOBS->WORKER_COORDINATION_PING_DELAY );
	state uint64_t step = 0;
	state PromiseStream<Future<Void>> addActor;
	state Future<ErrorOr<Void>> error = errorOr( actorCollection( addActor.getFuture() ) );

	auto pSelf = &self;
	addActor.send( failureDetectionServer( self.id, &self.db, interf.clientInterface.failureMonitoring.getFuture() ) );
	addActor.send( clusterWatchDatabase( &self, &self.db ) );  // Start the master database
	addActor.send( self.updateWorkerList.init( self.db.db ) );
	addActor.send( statusServer( interf.clientInterface.databaseStatus.getFuture(), &self, coordinators));
	addActor.send( timeKeeper(&self) );
	addActor.send( monitorProcessClasses(&self) );
	addActor.send( monitorClientTxnInfoConfigs(&self.db) );
	addActor.send( updatedChangingDatacenters(&self) );
	addActor.send( updatedChangedDatacenters(&self) );
	addActor.send( updateDatacenterVersionDifference(&self) );
	//printf("%s: I am the cluster controller\n", g_network->getLocalAddress().toString().c_str());

	loop choose {
		when( ErrorOr<Void> err = wait( error ) ) {
			if (err.isError()) {
				endRole(Role::CLUSTER_CONTROLLER, interf.id(), "Stop Received Error", false, err.getError());
			}
			else {
				endRole(Role::CLUSTER_CONTROLLER, interf.id(), "Stop Received Signal", true);
			}

			// We shut down normally even if there was a serious error (so this fdbserver may be re-elected cluster controller)
			return Void();
		}
		when( OpenDatabaseRequest req = waitNext( interf.clientInterface.openDatabase.getFuture() ) ) {
			addActor.send( clusterOpenDatabase( &self.db, req.knownClientInfoID, req.issues.toString(), req.supportedVersions, req.traceLogGroup, req.reply ) );
		}
		when( RecruitFromConfigurationRequest req = waitNext( interf.recruitFromConfiguration.getFuture() ) ) {
			addActor.send( clusterRecruitFromConfiguration( &self, req ) );
		}
		when( RecruitRemoteFromConfigurationRequest req = waitNext( interf.recruitRemoteFromConfiguration.getFuture() ) ) {
			addActor.send( clusterRecruitRemoteFromConfiguration( &self, req ) );
		}
		when( RecruitStorageRequest req = waitNext( interf.recruitStorage.getFuture() ) ) {
			clusterRecruitStorage( &self, req );
		}
		when( RegisterWorkerRequest req = waitNext( interf.registerWorker.getFuture() ) ) {
			registerWorker( req, &self );
		}
		when( GetWorkersRequest req = waitNext( interf.getWorkers.getFuture() ) ) {
			vector<std::pair<WorkerInterface, ProcessClass>> workers;

			auto masterAddr = self.db.serverInfo->get().master.address();
			for(auto& it : self.id_worker) {
				if ( (req.flags & GetWorkersRequest::NON_EXCLUDED_PROCESSES_ONLY) && self.db.config.isExcludedServer(it.second.interf.address()) ) {
					continue;
				}

				if ( (req.flags & GetWorkersRequest::TESTER_CLASS_ONLY) && it.second.processClass.classType() != ProcessClass::TesterClass ) {
					continue;
				}

				workers.push_back(std::make_pair(it.second.interf, it.second.processClass));
			}

			req.reply.send( workers );
		}
		when( GetClientWorkersRequest req = waitNext( interf.clientInterface.getClientWorkers.getFuture() ) ) {
			vector<ClientWorkerInterface> workers;
			for(auto& it : self.id_worker) {
				if (it.second.processClass.classType() != ProcessClass::TesterClass) {
					workers.push_back(it.second.interf.clientInterface);
				}
			}
			req.reply.send(workers);
		}
		when( ForceRecoveryRequest req = waitNext( interf.clientInterface.forceRecovery.getFuture() ) ) {
			if(self.db.masterRegistrationCount == 0 || self.db.serverInfo->get().recoveryState <= RecoveryState::RECRUITING) {
				if (!self.db.forceMasterFailure.isSet()) {
					self.db.forceRecovery = true;
					self.db.forceMasterFailure.send( Void() );
				}
			}
			req.reply.send(Void());
		}
		when( wait( coordinationPingDelay ) ) {
			CoordinationPingMessage message(self.id, step++);
			for(auto& it : self.id_worker)
				it.second.interf.coordinationPing.send(message);
			coordinationPingDelay = delay( SERVER_KNOBS->WORKER_COORDINATION_PING_DELAY );
			TraceEvent("CoordinationPingSent", self.id).detail("TimeStep", message.timeStep);
		}
		when( RegisterMasterRequest req = waitNext( interf.registerMaster.getFuture() ) ) {
			clusterRegisterMaster( &self, req );
		}
		when( GetServerDBInfoRequest req = waitNext( interf.getServerDBInfo.getFuture() ) ) {
			addActor.send( clusterGetServerInfo( &self.db, req.knownServerInfoID, req.issues.toString(), req.incompatiblePeers, req.reply ) );
		}
		when( wait( leaderFail ) ) {
			// We are no longer the leader if this has changed.
			endRole(Role::CLUSTER_CONTROLLER, interf.id(), "Leader Replaced", true);
			TEST(true); // Lost Cluster Controller Role
			return Void();
		}
		when( ReplyPromise<Void> ping = waitNext( interf.clientInterface.ping.getFuture() ) ) {
			ping.send( Void() );
		}
	}
}

ACTOR Future<Void> clusterController( ServerCoordinators coordinators, Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> currentCC, bool hasConnected, Reference<AsyncVar<ClusterControllerPriorityInfo>> asyncPriorityInfo, LocalityData locality ) {
	loop {
		state ClusterControllerFullInterface cci;
		state bool inRole = false;
		cci.initEndpoints();
		try {
			//Register as a possible leader; wait to be elected
			state Future<Void> leaderFail = tryBecomeLeader( coordinators, cci, currentCC, hasConnected, asyncPriorityInfo );

			while (!currentCC->get().present() || currentCC->get().get() != cci) {
				choose {
					when( wait(currentCC->onChange()) ) {}
					when( wait(leaderFail) ) { ASSERT(false); throw internal_error(); }
				}
			}

			hasConnected = true;
			startRole(Role::CLUSTER_CONTROLLER, cci.id(), UID());
			inRole = true;

			wait( clusterControllerCore( cci, leaderFail, coordinators, locality ) );
		} catch(Error& e) {
			if (inRole)
				endRole(Role::CLUSTER_CONTROLLER, cci.id(), "Error", e.code() == error_code_actor_cancelled || e.code() == error_code_coordinators_changed, e);
			else
				TraceEvent( e.code() == error_code_coordinators_changed ? SevInfo : SevError, "ClusterControllerCandidateError", cci.id()).error(e);
			throw;
		}
	}
}

ACTOR Future<Void> clusterController( Reference<ClusterConnectionFile> connFile, Reference<AsyncVar<Optional<ClusterControllerFullInterface>>> currentCC, Reference<AsyncVar<ClusterControllerPriorityInfo>> asyncPriorityInfo, Future<Void> recoveredDiskFiles, LocalityData locality ) {
	wait(recoveredDiskFiles);
	state bool hasConnected = false;
	loop {
		try {
			ServerCoordinators coordinators( connFile );
			wait( clusterController( coordinators, currentCC, hasConnected, asyncPriorityInfo, locality ) );
		} catch( Error &e ) {
			if( e.code() != error_code_coordinators_changed )
				throw; // Expected to terminate fdbserver
		}

		hasConnected = true;
	}
}
