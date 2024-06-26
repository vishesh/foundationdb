fdb_find_sources(FDBSERVER_SRCS)

if(${COROUTINE_IMPL} STREQUAL libcoro)
  list(APPEND FDBSERVER_SRCS coroimpl/CoroFlowCoro.actor.cpp)
else()
  list(APPEND FDBSERVER_SRCS coroimpl/CoroFlow.actor.cpp)
endif()


if(WITH_ROCKSDB)
  include(CompileRocksDB)
  # CompileRocksDB sets `lz4_LIBRARIES` to be the shared lib, we want to link
  # statically, so find the static library here.
  find_library(lz4_STATIC_LIBRARIES
    NAMES liblz4.a REQUIRED)
  if(WITH_LIBURING)
    find_package(uring)
  endif()
endif()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/workloads)

add_flow_target(EXECUTABLE NAME fdbserver SRCS ${FDBSERVER_SRCS})

target_include_directories(fdbserver PRIVATE
  ${CMAKE_SOURCE_DIR}/bindings/c
  ${CMAKE_BINARY_DIR}/bindings/c
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/include)
if (WITH_ROCKSDB)
  add_dependencies(fdbserver rocksdb)
  if(WITH_LIBURING)
    target_include_directories(fdbserver PRIVATE ${ROCKSDB_INCLUDE_DIR} ${uring_INCLUDE_DIR})
    target_link_libraries(fdbserver PRIVATE fdbclient metacluster sqlite ${ROCKSDB_LIBRARIES} ${uring_LIBRARIES} ${lz4_STATIC_LIBRARIES})
    target_compile_definitions(fdbserver PRIVATE BOOST_ASIO_HAS_IO_URING=1 BOOST_ASIO_DISABLE_EPOLL=1)
  else()
    target_include_directories(fdbserver PRIVATE ${ROCKSDB_INCLUDE_DIR})
    target_link_libraries(fdbserver PRIVATE fdbclient metacluster sqlite ${ROCKSDB_LIBRARIES} ${lz4_STATIC_LIBRARIES})
  endif()
else()
  target_link_libraries(fdbserver PRIVATE fdbclient metacluster sqlite)
endif()

if(USE_JEMALLOC)
  target_include_directories(fdbserver INTERFACE $<TARGET_PROPERTY:jemalloc::jemalloc, INTERFACE_INCLUDE_DIRECTORIES>)
  target_link_libraries(fdbserver PRIVATE jemalloc::jemalloc)
endif()

target_link_libraries(fdbserver PRIVATE toml11_target rapidjson)
if(WITH_ROCKSDB)
  target_compile_definitions(fdbserver PRIVATE WITH_ROCKSDB)
endif()

if(GPERFTOOLS_FOUND)
  target_link_libraries(fdbserver PRIVATE gperftools)
endif()

if(NOT OPEN_FOR_IDE)
  if(GENERATE_DEBUG_PACKAGES)
    fdb_install(TARGETS fdbserver DESTINATION sbin COMPONENT server)
  else()
    add_custom_target(prepare_fdbserver_install ALL DEPENDS strip_only_fdbserver)
    fdb_install(PROGRAMS ${CMAKE_BINARY_DIR}/packages/bin/fdbserver DESTINATION sbin COMPONENT server)
  endif()
endif()
