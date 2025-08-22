/*
 * FDB.swift
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2016-2025 Apple Inc. and the FoundationDB project authors
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
import CFoundationDB
import Foundation

public class FDB {
    public enum APIVersion {
        public static let current: Int32 = 740
    }

    public static func initialize(version: Int32 = APIVersion.current) throws {
        try FDBClient.initialize(version: version)
    }

    public static func createDatabase(clusterFilePath: String? = nil) throws -> FDBDatabase {
        var database: OpaquePointer?
        let error = fdb_create_database(clusterFilePath, &database)
        if error != 0 {
            throw FDBError(code: error)
        }

        guard let db = database else {
            throw FDBError(code: 2000) // internal_error
        }

        return FDBDatabase(database: db)
    }
}

private class FDBClient {
    static let shared = FDBClient()

    private static var networkSetup = false
    private static var networkThread: Thread?

    static func initialize(version: Int32) throws {
        try selectAPIVersion(version)
        try setupNetwork()
        startNetwork()
    }

    static func selectAPIVersion(_ version: Int32) throws {
        let error = fdb_select_api_version_impl(version, FDB_API_VERSION)
        if error != 0 {
            throw FDBError(code: 2201)
        }
    }

    static func setupNetwork() throws {
        guard !networkSetup else {
            throw FDBError(code: 2201)
        }

        let error = fdb_setup_network()
        if error != 0 {
            throw FDBError(code: error)
        }

        networkSetup = true
    }

    static func startNetwork() {
        guard networkSetup else {
            fatalError("Network must be setup before starting network thread")
        }

        networkThread = Thread {
            let error = fdb_run_network()
            if error != 0 {
                print("Network thread error: \(FDBError(code: error).description)")
            }
        }
        networkThread?.start()
    }

    static func stopNetwork() throws {
        let error = fdb_stop_network()
        if error != 0 {
            throw FDBError(code: error)
        }

        networkThread?.cancel()
        networkThread = nil
        networkSetup = false
    }
}
