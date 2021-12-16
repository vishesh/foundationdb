/*
 * LogSystem.cpp
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

#include "fdbserver/LogSystem.h"
#include "fdbserver/LogProtocolMessage.h"
#include "fdbserver/SpanContextMessage.h"
#include "fdbserver/ptxn/MessageSerializer.h"
#include "fdbserver/ptxn/MessageTypes.h"
#include "fdbserver/ptxn/test/FakeLogSystem.h"
#include "fdbserver/TagPartitionedLogSystem.actor.h"
#include "fdbserver/TeamPartitionedLogSystem.actor.h"
#include "flow/Error.h"
#include "flow/serialize.h"

#ifndef __INTEL_COMPILER
#pragma region LogSet
#endif

std::string LogSet::logRouterString() {
	std::string result;
	for (int i = 0; i < logRouters.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += logRouters[i]->get().id().toString();
	}
	return result;
}

bool LogSet::hasLogRouter(UID id) const {
	for (const auto& router : logRouters) {
		if (router->get().id() == id) {
			return true;
		}
	}
	return false;
}

bool LogSet::hasBackupWorker(UID id) const {
	for (const auto& worker : backupWorkers) {
		if (worker->get().id() == id) {
			return true;
		}
	}
	return false;
}

std::string LogSet::logServerString() {
	std::string result;
	for (int i = 0; i < logServers.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += logServers[i]->get().id().toString();
	}
	return result;
}

void LogSet::populateSatelliteTagLocations(int logRouterTags, int oldLogRouterTags, int txsTags, int oldTxsTags) {
	satelliteTagLocations.clear();
	satelliteTagLocations.resize(std::max({ logRouterTags, oldLogRouterTags, txsTags, oldTxsTags }) + 1);

	std::map<int, int> server_usedBest;
	std::set<std::pair<int, int>> used_servers;
	for (int i = 0; i < tLogLocalities.size(); i++) {
		used_servers.insert(std::make_pair(0, i));
	}

	Reference<LocalitySet> serverSet = Reference<LocalitySet>(new LocalityMap<std::pair<int, int>>());
	LocalityMap<std::pair<int, int>>* serverMap = (LocalityMap<std::pair<int, int>>*)serverSet.getPtr();
	std::vector<std::pair<int, int>> resultPairs;
	for (int loc = 0; loc < satelliteTagLocations.size(); loc++) {
		int team = loc;
		if (loc < logRouterTags) {
			team = loc + 1;
		} else if (loc == logRouterTags) {
			team = 0;
		}

		bool teamComplete = false;
		alsoServers.resize(1);
		serverMap->clear();
		resultPairs.clear();
		for (auto& used_idx : used_servers) {
			auto entry = serverMap->add(tLogLocalities[used_idx.second], &used_idx);
			if (!resultPairs.size()) {
				resultPairs.push_back(used_idx);
				alsoServers[0] = entry;
			}

			resultEntries.clear();
			if (serverSet->selectReplicas(tLogPolicy, alsoServers, resultEntries)) {
				for (auto& entry : resultEntries) {
					resultPairs.push_back(*serverMap->getObject(entry));
				}
				int firstBestUsed = server_usedBest[resultPairs[0].second];
				for (int i = 1; i < resultPairs.size(); i++) {
					int thisBestUsed = server_usedBest[resultPairs[i].second];
					if (thisBestUsed < firstBestUsed) {
						std::swap(resultPairs[0], resultPairs[i]);
						firstBestUsed = thisBestUsed;
					}
				}
				server_usedBest[resultPairs[0].second]++;

				for (auto& res : resultPairs) {
					satelliteTagLocations[team].push_back(res.second);
					used_servers.erase(res);
					res.first++;
					used_servers.insert(res);
				}
				teamComplete = true;
				break;
			}
		}
		ASSERT(teamComplete);
	}

	checkSatelliteTagLocations();
}

void LogSet::checkSatelliteTagLocations() {
	std::vector<int> usedBest;
	std::vector<int> used;
	usedBest.resize(tLogLocalities.size());
	used.resize(tLogLocalities.size());
	for (auto team : satelliteTagLocations) {
		usedBest[team[0]]++;
		for (auto loc : team) {
			used[loc]++;
		}
	}

	int minUsedBest = satelliteTagLocations.size();
	int maxUsedBest = 0;
	for (auto i : usedBest) {
		minUsedBest = std::min(minUsedBest, i);
		maxUsedBest = std::max(maxUsedBest, i);
	}

	int minUsed = satelliteTagLocations.size();
	int maxUsed = 0;
	for (auto i : used) {
		minUsed = std::min(minUsed, i);
		maxUsed = std::max(maxUsed, i);
	}

	bool foundDuplicate = false;
	std::set<Optional<Key>> zones;
	std::set<Optional<Key>> dcs;
	for (auto& loc : tLogLocalities) {
		if (zones.count(loc.zoneId())) {
			foundDuplicate = true;
			break;
		}
		zones.insert(loc.zoneId());
		dcs.insert(loc.dcId());
	}
	bool moreThanOneDC = dcs.size() > 1 ? true : false;

	TraceEvent(((maxUsed - minUsed > 1) || (maxUsedBest - minUsedBest > 1))
	               ? (g_network->isSimulated() && !foundDuplicate && !moreThanOneDC ? SevError : SevWarnAlways)
	               : SevInfo,
	           "CheckSatelliteTagLocations")
	    .detail("MinUsed", minUsed)
	    .detail("MaxUsed", maxUsed)
	    .detail("MinUsedBest", minUsedBest)
	    .detail("MaxUsedBest", maxUsedBest)
	    .detail("DuplicateZones", foundDuplicate)
	    .detail("NumOfDCs", dcs.size());
}

int LogSet::bestLocationFor(Tag tag) {
	if (locality == tagLocalitySatellite) {
		return satelliteTagLocations[tag == txsTag ? 0 : tag.id + 1][0];
	}

	// the following logic supports upgrades from 5.X
	if (tag == txsTag)
		return txsTagOld % logServers.size();
	return tag.id % logServers.size();
}

void LogSet::updateLocalitySet(std::vector<LocalityData> const& localities) {
	LocalityMap<int>* logServerMap;

	logServerSet = Reference<LocalitySet>(new LocalityMap<int>());
	logServerMap = (LocalityMap<int>*)logServerSet.getPtr();

	logEntryArray.clear();
	logEntryArray.reserve(localities.size());
	logIndexArray.clear();
	logIndexArray.reserve(localities.size());

	for (int i = 0; i < localities.size(); i++) {
		logIndexArray.push_back(i);
		logEntryArray.push_back(logServerMap->add(localities[i], &logIndexArray.back()));
	}
}

bool LogSet::satisfiesPolicy(const std::vector<LocalityEntry>& locations) {
	resultEntries.clear();

	// Run the policy, assert if unable to satify
	bool result = logServerSet->selectReplicas(tLogPolicy, locations, resultEntries);
	ASSERT(result);

	return resultEntries.size() == 0;
}

void LogSet::getPushLocations(VectorRef<Tag> tags, std::vector<int>& locations, int locationOffset, bool allLocations) {
	if (locality == tagLocalitySatellite) {
		for (auto& t : tags) {
			if (t == txsTag || t.locality == tagLocalityTxs || t.locality == tagLocalityLogRouter) {
				for (int loc : satelliteTagLocations[t == txsTag ? 0 : t.id + 1]) {
					locations.push_back(locationOffset + loc);
				}
			}
		}
		uniquify(locations);
		return;
	}

	newLocations.clear();
	alsoServers.clear();
	resultEntries.clear();

	if (allLocations) {
		// special handling for allLocations
		TraceEvent("AllLocationsSet");
		for (int i = 0; i < logServers.size(); i++) {
			newLocations.push_back(i);
		}
	} else {
		for (auto& t : tags) {
			if (locality == tagLocalitySpecial || t.locality == locality || t.locality < 0) {
				newLocations.push_back(bestLocationFor(t));
			}
		}
	}

	uniquify(newLocations);

	if (newLocations.size())
		alsoServers.reserve(newLocations.size());

	// Convert locations to the also servers
	for (auto location : newLocations) {
		locations.push_back(locationOffset + location);
		alsoServers.push_back(logEntryArray[location]);
	}

	// Run the policy, assert if unable to satify
	bool result = logServerSet->selectReplicas(tLogPolicy, alsoServers, resultEntries);
	ASSERT(result);

	// Add the new servers to the location array
	LocalityMap<int>* logServerMap = (LocalityMap<int>*)logServerSet.getPtr();
	for (auto entry : resultEntries) {
		locations.push_back(locationOffset + *logServerMap->getObject(entry));
	}
	//TraceEvent("GetPushLocations").detail("Policy", tLogPolicy->info())
	//	.detail("Results", locations.size()).detail("Selection", logServerSet->size())
	//	.detail("Included", alsoServers.size()).detail("Duration", timer() - t);
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

#ifndef __INTEL_COMPILER
#pragma region LogPushData
#endif

void LogPushData::addTxsTag() {
	if (logSystem->getTLogVersion() >= TLogVersion::V4) {
		next_message_tags.push_back(logSystem->getRandomTxsTag());
	} else {
		next_message_tags.push_back(txsTag);
	}
}

void LogPushData::addTransactionInfo(SpanID const& context) {
	TEST(!spanContext.isValid()); // addTransactionInfo with invalid SpanID
	spanContext = context;
	writtenLocations.clear();
}

void LogPushData::writeMessage(StringRef rawMessageWithoutLength, bool usePreviousLocations) {
	if (!usePreviousLocations) {
		prev_tags.clear();
		if (logSystem->hasRemoteLogs()) {
			prev_tags.push_back(logSystem->getRandomRouterTag());
		}
		for (auto& tag : next_message_tags) {
			prev_tags.push_back(tag);
		}
		msg_locations.clear();
		logSystem->getPushLocations(prev_tags, msg_locations);
		next_message_tags.clear();
	}
	uint32_t subseq = this->subsequence++;
	uint32_t msgsize =
	    rawMessageWithoutLength.size() + sizeof(subseq) + sizeof(uint16_t) + sizeof(Tag) * prev_tags.size();
	for (int loc : msg_locations) {
		BinaryWriter& wr = messagesWriter[loc];
		wr << msgsize << subseq << uint16_t(prev_tags.size());
		for (auto& tag : prev_tags)
			wr << tag;
		wr.serializeBytes(rawMessageWithoutLength);
	}
}

std::vector<Standalone<StringRef>> LogPushData::getAllMessages() {
	std::vector<Standalone<StringRef>> results;
	results.reserve(messagesWriter.size());
	for (int loc = 0; loc < messagesWriter.size(); loc++) {
		results.push_back(getMessages(loc));
	}
	return results;
}

void LogPushData::recordEmptyMessage(int loc, const Standalone<StringRef>& value) {
	if (!isEmptyMessage[loc]) {
		BinaryWriter w(AssumeVersion(g_network->protocolVersion()));
		Standalone<StringRef> v = w.toValue();
		if (value.size() > v.size()) {
			isEmptyMessage[loc] = true;
		}
	}
}

float LogPushData::getEmptyMessageRatio() const {
	auto count = std::count(isEmptyMessage.begin(), isEmptyMessage.end(), false);
	ASSERT_WE_THINK(isEmptyMessage.size() > 0);
	return 1.0 * count / isEmptyMessage.size();
}

bool LogPushData::writeTransactionInfo(int location, uint32_t subseq) {
	if (!FLOW_KNOBS->WRITE_TRACING_ENABLED || logSystem->getTLogVersion() < TLogVersion::V6 ||
	    writtenLocations.count(location) != 0) {
		return false;
	}

	TEST(true); // Wrote SpanContextMessage to a transaction log
	writtenLocations.insert(location);

	BinaryWriter& wr = messagesWriter[location];
	SpanContextMessage contextMessage(spanContext);

	int offset = wr.getLength();
	wr << uint32_t(0) << subseq << uint16_t(prev_tags.size());
	for (auto& tag : prev_tags)
		wr << tag;
	wr << contextMessage;
	int length = wr.getLength() - offset;
	*(uint32_t*)((uint8_t*)wr.getData() + offset) = length - sizeof(uint32_t);
	return true;
}

void LogPushData::addTLogGroups(const std::vector<TLogGroupRef>& groups, Version commitVersion) {
	for (const auto& group : groups) {
		pGroupMessageBuilders->emplace(group->id(),
		                               std::make_shared<ptxn::ProxySubsequencedMessageSerializer>(commitVersion));
	}
}

std::unordered_map<ptxn::TLogGroupID, ptxn::SerializedTeamData> LogPushData::getGroupMutations(
    const std::set<ptxn::TLogGroupID>& groups) {
	std::unordered_map<ptxn::TLogGroupID, ptxn::SerializedTeamData> results;
	for (const auto& [group, serializer] : *pGroupMessageBuilders) {
		auto teamData = serializer->getAllSerialized();
		results.emplace(group, teamData);
	}
	return results;
}

// TODO: we deserialize mutations sent from Resolvers and serialized to pGroupMessageBuilders.
// It would be nice that ProxySubsequencedMessageSerializer can be set with
// serialized data.
void LogPushData::setGroupMutations(
    const std::map<ptxn::TLogGroupID, std::unordered_map<ptxn::StorageTeamID, StringRef>>& groupMutations,
    Version commitVersion) {
	for (const auto& [group, teamData] : groupMutations) {
		auto it = pGroupMessageBuilders->find(group);
		if (it == pGroupMessageBuilders->end()) {
			it = pGroupMessageBuilders
			         ->emplace(group, std::make_shared<ptxn::ProxySubsequencedMessageSerializer>(commitVersion))
			         .first;
		}
		auto& writer = it->second;
		for (const auto& [team, mutations] : teamData) {
			ptxn::SubsequencedMessageDeserializer deserializer(mutations);
			for (const auto& item : deserializer) {
				if (item.message.getType() == ptxn::Message::Type::SPAN_CONTEXT_MESSAGE) {
					writer->writeTeamSpanContext(std::get<SpanContextMessage>(item.message), team);
				} else if (item.message.getType() == ptxn::Message::Type::MUTATION_REF) {
					writer->write(std::get<MutationRef>(item.message), team);
				} else if (item.message.getType() == ptxn::Message::Type::LOG_PROTOCOL_MESSAGE) {
					writer->write(std::get<LogProtocolMessage>(item.message), team);
				} else {
					UNREACHABLE();
				}
			}
		}
	}
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

#ifndef __INTEL_COMPILER
#pragma region ILogSystem
#endif

Future<Void> ILogSystem::recoverAndEndEpoch(Reference<AsyncVar<Reference<ILogSystem>>> const& outLogSystem,
                                            UID const& dbgid,
                                            DBCoreState const& oldState,
                                            FutureStream<TLogRejoinRequest> const& rejoins,
                                            LocalityData const& locality,
                                            bool* forceRecovery) {
	return TagPartitionedLogSystem::recoverAndEndEpoch(outLogSystem, dbgid, oldState, rejoins, locality, forceRecovery);
}

Reference<ILogSystem> ILogSystem::fromLogSystemConfig(UID const& dbgid,
                                                      struct LocalityData const& locality,
                                                      struct LogSystemConfig const& conf,
                                                      bool excludeRemote,
                                                      bool useRecoveredAt,
                                                      Optional<PromiseStream<Future<Void>>> addActor) {
	switch (conf.logSystemType) {
	case LogSystemType::empty:
		TraceEvent(SevDebug, "ILogSystem::fromLogSystemConfig").detail("LogSystemType", "empty");
		return Reference<ILogSystem>();
	case LogSystemType::tagPartitioned:
		TraceEvent(SevDebug, "ILogSystem::fromLogSystemConfig").detail("LogSystemType", "tagPartitioned");
		return TagPartitionedLogSystem::fromLogSystemConfig(
		    dbgid, locality, conf, excludeRemote, useRecoveredAt, addActor);
	case LogSystemType::teamPartitioned:
		TraceEvent(SevDebug, "ILogSystem::fromLogSystemConfig").detail("LogSystemType", "teamPartitioned");
		return ptxn::TeamPartitionedLogSystem::fromLogSystemConfig(
		    dbgid, locality, conf, excludeRemote, useRecoveredAt, addActor);
	case LogSystemType::fake:
		TraceEvent(SevDebug, "ILogSystem::fromLogSystemConfig").detail("LogSystemType", "fake");
		return makeReference<ptxn::test::FakeLogSystem>(dbgid);
	case LogSystemType::fake_FakePeekCursor:
		TraceEvent(SevDebug, "ILogSystem::fromLogSystemConfig").detail("LogSystemType", "fake_FakePeekCursor");
		return makeReference<ptxn::test::FakeLogSystem_CustomPeekCursor>(dbgid);
	default:
		throw internal_error();
	}
}

Reference<ILogSystem> ILogSystem::fromOldLogSystemConfig(UID const& dbgid,
                                                         struct LocalityData const& locality,
                                                         struct LogSystemConfig const& conf) {
	switch (conf.logSystemType) {
	case LogSystemType::empty:
		TraceEvent(SevDebug, "ILogSystem::fromOldLogSystemConfig").detail("LogSystemType", "empty");
		return Reference<ILogSystem>();
	case LogSystemType::tagPartitioned:
		TraceEvent(SevDebug, "ILogSystem::fromOldLogSystemConfig").detail("LogSystemType", "tagPartitioned");
		return TagPartitionedLogSystem::fromOldLogSystemConfig(dbgid, locality, conf);
	case LogSystemType::teamPartitioned:
		TraceEvent(SevDebug, "ILogSystem::fromOldLogSystemConfig").detail("LogSystemType", "teamPartitioned");
		throw internal_error_msg("Not supported yet");
	case LogSystemType::fake:
		TraceEvent(SevDebug, "ILogSystem::fromOldLogSystemConfig").detail("LogSystemType", "fake");
		return makeReference<ptxn::test::FakeLogSystem>(dbgid);
	case LogSystemType::fake_FakePeekCursor:
		TraceEvent(SevDebug, "ILogSystem::fromOldLogSystemConfig").detail("LogSystemType", "fake_FakePeekCursor");
		return makeReference<ptxn::test::FakeLogSystem_CustomPeekCursor>(dbgid);
	default:
		throw internal_error();
	}
}

Reference<ILogSystem> ILogSystem::fromServerDBInfo(UID const& dbgid,
                                                   ServerDBInfo const& dbInfo,
                                                   bool useRecoveredAt,
                                                   Optional<PromiseStream<Future<Void>>> addActor) {
	return fromLogSystemConfig(dbgid, dbInfo.myLocality, dbInfo.logSystemConfig, false, useRecoveredAt, addActor);
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

void LogPushData::setMutations(uint32_t totalMutations, VectorRef<StringRef> mutations) {
	ASSERT_EQ(subsequence, 1);
	subsequence = totalMutations + 1; // set to next mutation number

	ASSERT_EQ(messagesWriter.size(), mutations.size());
	BinaryWriter w(AssumeVersion(g_network->protocolVersion()));
	Standalone<StringRef> v = w.toValue();
	const int header = v.size();
	for (int i = 0; i < mutations.size(); i++) {
		BinaryWriter& wr = messagesWriter[i];
		wr.serializeBytes(mutations[i].substr(header));
	}
}
