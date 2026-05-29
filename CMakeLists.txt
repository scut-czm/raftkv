# TODO: cmake_minimum_required / project(kv_server CXX C) / C++17
# TODO: find_package(Protobuf/gflags/GTest)
# TODO: add_subdirectory(braft)
# TODO: protobuf_generate_cpp(kv.proto + schema.proto + row.proto)
# TODO: add_library(sql_engine STATIC —— 所有 src/sql/*.cc + operators/*.cc + proto)
# TODO: add_executable(kv_server / kv_client) + 各 Day 单测可执行目标
# TODO: Day 17: sql_client / sql_end_to_end_test（依赖 sql_engine + raftkv_client）
# TODO: enable_testing() + add_test() 注册所有单测
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

# ── SQL Proto 编译（Day 11/13/14）────────────────────────────────────────
set(SQL_PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/schema.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/row.proto
)
protobuf_generate_cpp(SQL_PROTO_SRCS SQL_PROTO_HDRS ${SQL_PROTO_FILES})

# ── SQL 引擎核心静态库 ────────────────────────────────────────────────────
set(SQL_ENGINE_SRCS
    src/sql/lexer.cc
    src/sql/ast.cc
    src/sql/parser.cc
    src/sql/logical_plan.cc
    src/sql/planner.cc
    src/sql/logical_optimizer.cc
    src/sql/table_schema.cc
    src/sql/schema_manager.cc
    src/sql/row_codec.cc
    src/sql/row_id_allocator.cc
    src/sql/operators/table_scan_operator.cc
    src/sql/operators/filter_operator.cc
    src/sql/operators/project_operator.cc
    src/sql/operators/aggregate_operator.cc
    src/sql/operators/limit_operator.cc
    src/sql/physical_planner.cc
    src/sql/sql_executor.cc
    ${SQL_PROTO_SRCS}
)

set(SQL_ADAPTER_SRCS
    src/sql/raft_kv_client_adapter.cc
    src/client/kv_client.cc
    ${PROTO_SRCS}
)

add_library(sql_engine STATIC ${SQL_ENGINE_SRCS})
target_include_directories(sql_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(sql_engine
    ${PROTOBUF_LIBRARIES} pthread
)

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

# ── Day 9: parser_test ───────────────────────────────────────────────────
add_executable(parser_test
    tests/unit/parser_test.cc
    src/sql/lexer.cc
    src/sql/ast.cc
    src/sql/parser.cc
)
target_include_directories(parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(parser_test
    GTest::GTest GTest::Main pthread
)