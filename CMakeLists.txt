cmake_minimum_required(VERSION 2.8.12)
project(kv_server CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE "RelWithDebInfo")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找依赖
find_package(Protobuf REQUIRED)
find_package(gflags REQUIRED)
find_package(GTest REQUIRED)

# 添加 braft 子项目（本地开发可用 -DBRAFT_SOURCE_DIR=../../braft_learning/braft 覆盖）
if(NOT DEFINED BRAFT_SOURCE_DIR)
  set(BRAFT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/braft)
endif()
add_subdirectory(${BRAFT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/braft_build)

# 查找第三方库
find_library(BRPC_LIB    brpc    HINTS /usr/local/lib ${CMAKE_PREFIX_PATH}/lib)
find_library(ROCKSDB_LIB rocksdb HINTS /usr/local/lib ${CMAKE_PREFIX_PATH}/lib)

include_directories(/usr/local/include)
include_directories(${CMAKE_PREFIX_PATH}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${BRAFT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/braft_build)

# 生成 Protobuf 代码（proto/ 子目录）
set(PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/kv.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# ── Day 1: storage 单测 ──────────────────────────────────────────────────
add_executable(storage_test
    tests/unit/storage_test.cc
    src/storage/rocksdb_storage.cc
)

target_include_directories(storage_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)

target_link_libraries(storage_test
    ${ROCKSDB_LIB}
    GTest::GTest GTest::Main pthread snappy z lz4
)

# ── Day 2+: kv_server（源文件实现后可 make kv_server）──────────────────
add_executable(kv_server
    server/server_main.cc
    src/raft/kv_state_machine.cc
    src/service/kv_service.cc
    src/storage/rocksdb_storage.cc
    ${PROTO_SRCS}
)
target_link_libraries(kv_server
    braft-static ${BRPC_LIB} ${ROCKSDB_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto
    leveldb snappy z lz4
)

# ── kv_client（命令行测试客户端）────────────────────────────────────────
add_executable(kv_client
    client/client_main.cc
    src/client/kv_client.cc
    ${PROTO_SRCS}
)
target_link_libraries(kv_client
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
)

# ── Day 2: state_machine_test ────────────────────────────────────────────

add_executable(state_machine_test
    tests/unit/state_machine_test.cc
    src/raft/kv_state_machine.cc
    src/storage/rocksdb_storage.cc
    ${PROTO_SRCS}
)
target_link_libraries(state_machine_test
    braft-static ${BRPC_LIB} ${ROCKSDB_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto
    GTest::GTest GTest::Main snappy z lz4
)


# ── Day 3: client_test ─────────────────────────────────────────────────
add_executable(client_test 
    tests/unit/client_test.cc
    src/client/kv_client.cc
    ${PROTO_SRCS}
)

target_link_libraries(client_test
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
    GTest::GTest GTest::Main
)

# ── Day 5: perf_test（性能基准测试）────────────────────────────────────

add_executable(perf_test
    tests/integration/perf_test.cc
    src/client/kv_client.cc
    ${PROTO_SRCS}
)

target_link_libraries(perf_test
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
)

# ── ctest 注册 ────────────────────────────────────────────────────────────
enable_testing()
add_test(NAME storage_test     COMMAND storage_test)
add_test(NAME state_machine_test COMMAND state_machine_test)
add_test(NAME client_test      COMMAND client_test)