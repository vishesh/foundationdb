/*
 * TestStorageServer.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2021 Apple Inc. and the FoundationDB project authors
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

#include "fdbserver/ptxn/test/TestStorageServer.actor.h"

#include "fdbclient/FDBTypes.h"
#include "fdbserver/ptxn/test/Driver.h"
#include "fdbserver/ptxn/test/Utils.h"
#include "flow/UnitTest.h"

#include "flow/actorcompiler.h" // This must be the last #include

namespace ptxn::test {

TestStorageServerPullOptions::TestStorageServerPullOptions(const UnitTestParameters& params)
  : numTLogGroups(params.getInt("numTLogGroups").orDefault(DEFAULT_TLOG_GROUPS)),
    numStorageTeams(params.getInt("numStorageTeams").orDefault(DEFAULT_STORAGE_TEAMS)) {}

} // namespace ptxn::test

TEST_CASE("fdbserver/ptxn/test/StorageServerPull") {
	state ptxn::test::TestEnvironment testEnvironment;
	state ptxn::test::TestStorageServerPullOptions options(params);
	state ptxn::test::print::PrintTiming printTiming("fdbserver/ptxn/test/StorageServerPull");

	testEnvironment.initDriverContext()
	    .initTLogGroup(options.DEFAULT_TLOG_GROUPS, options.DEFAULT_STORAGE_TEAMS)
	    .initPtxnTLog(ptxn::MessageTransferModel::StorageServerActivelyPull, 1)
	    .initServerDBInfo()
	    .initPtxnStorageServer(testEnvironment.getTLogGroup().storageTeamIDs.size());

	std::vector<Future<InitializeStorageReply>> initializeStoreReplyFutures;
	std::transform(std::begin(ptxn::test::TestEnvironment::getStorageServers()->initializeStorageReplies),
	               std::end(ptxn::test::TestEnvironment::getStorageServers()->initializeStorageReplies),
	               std::back_inserter(initializeStoreReplyFutures),
	               [](ReplyPromise<InitializeStorageReply>& reply) { return reply.getFuture(); });
    wait(waitForAll(initializeStoreReplyFutures));

	printTiming << "All storage servers are ready" << std::endl;

	return Void();
}
