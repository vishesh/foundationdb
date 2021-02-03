Multi-version Client: Design & Implementation 
=============================================

FoundationDB client supports connecting to FDB clusters running at different
versions. The process of setting up multi-version client is [documented
here](https://apple.github.io/foundationdb/api-general.html#multi-version-client).
The user is just expected to provide the `libfdb_c.so.<version>` binaries for
each version, and FDB client will take care of using the correct library for
each request.

Terminology
-----------
- Local Library: The primary `libfdb_c.so` library used to start FDB client.
- External Library/Client: `libfdb_c.so` libraries loaded dynamically by the
  local library, which implements other versions of FDB client.

High Level Implementation
-------------------------
FDB client can be organized into following layers:

- **NativeAPI/ReadYourWrites**: Implements the actual logic to talk to the FDB
  database and do various database operations (including maintainance).
- **IClientApi**: [Discussed here](#IClientApi).
- **C Bindings**: Provides a low-level interface to FoundationDB. FDB language
  bindings are strictly written using these bindings. It is a thin wrapper on
  top of `IClientApi`. Visit `bindings/c/fdb_c.h`. By default
  `MultiVersionClient` implementation of `IClientApi` is used (`libfdb_c.h::API`).
- **FDB Language Bindings**: Implemented on top of C bindings using the C FFI
  libraries provided by the binding language. Language bindings are out-of-scope
  for this documented.

IClientApi
----------
`IClientApi` provides following interfaces:

- **IClientApi**:  Provides functions to  start/stop network loop, and create/open a database.
- **IDatabase**: Provides function to start a transaction.
- **ITransaction**: Provides functions to prepare a transaction, which is finally
  submitted to the FDB database server.
  
`IClientApi::*` interfaces has two implementations (1) ThreadSafe* (2)
MultiVersion*. Functions inside `NativeApi`/`RYWTransaction` look a lot similar
to the interface of `IClientApi::*`, with the primary distinction that the
`IClientApi` uses `ThreadSafe*` primitives (e.g. instead of returning `Future`
it returns `ThreadFuture`), which implies it should be safe to be used in a
multi-threaded environment.

TODO: What tasks in `Thread*` API tasks are scheduled on run loop?
TODO: Dig more into `ThreadSafe` primitives.

ThreadSafeApi
-------------

TODO

MultiVersionApi
---------------
`MultiVersionApi` is implemented on top of `ThreadSafeApi`, and is the default
implmentation used by `libfdb_c`. As name suggests, it is also the component
that implements the multi-version client support in FDB.

Source files of interest are:
- `bindings/fdbclient/MultiVersionTransaction.h`
- `bindings/fdbclient/MultiVersionTransaction.actor.cpp`
- `bindings/fdbclient/MultiVersionAssignmentVars.g`

