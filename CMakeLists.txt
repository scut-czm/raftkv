# TODO: cmake_minimum_required / project(kv_server CXX C) / C++17
# TODO: find_package(Protobuf/gflags/GTest)
# TODO: add_subdirectory(braft)
# TODO: protobuf_generate_cpp(kv.proto + schema.proto + row.proto)
# TODO: add_library(sql_engine STATIC —— 所有 src/sql/*.cc + operators/*.cc + proto)
# TODO: add_executable(kv_server / kv_client) + 各 Day 单测可执行目标
# TODO: Day 17: sql_client / sql_end_to_end_test（依赖 sql_engine + raftkv_client）
# TODO: enable_testing() + add_test() 注册所有单测
# 【修正点 1】提升最低版本到 3.5，消除 CMake 官方的 Deprecation 兼容性警告
cmake_minimum_required(VERSION 3.5)
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
# 【修正点 2】加入 SYSTEM 关键字，强制让编译器在编译 braft 源码时闭嘴，不打印任何内部警告
add_subdirectory(${BRAFT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/braft_build SYSTEM)

# 查找第三方库
find_library(BRPC_LIB    brpc    HINTS /usr/local/lib ${CMAKE_PREFIX_PATH}/lib)
find_library(ROCKSDB_LIB rocksdb HINTS /usr/local/lib ${CMAKE_PREFIX_PATH}/lib)

# 【修正点 3】将外部第三方库的头文件目录全部标记为 SYSTEM
# 这样当你的主项目代码 include 它们的头文件时，哪怕里面有类型不匹配或有符号警告，编译器也会自动忽略
include_directories(SYSTEM /usr/local/include)
include_directories(SYSTEM ${CMAKE_PREFIX_PATH}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)# 主项目源码目录保持普通引入，保留严格警告检测
include_directories(SYSTEM ${BRAFT_SOURCE_DIR}/src)
include_directories(SYSTEM ${CMAKE_CURRENT_BINARY_DIR}/braft_build)

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

# ── mvcc_codec 单测（纯编码逻辑，无 rocksdb 依赖）──────────────────────
add_executable(mvcc_codec_test
    tests/unit/mvcc_codec_test.cc
    src/storage/mvcc_codec.cc
)

target_include_directories(mvcc_codec_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mvcc_codec_test
    GTest::GTest GTest::Main pthread
)

# ── local_tso 单测（header-only，无外部依赖）─────────────────────
add_executable(local_tso_test
    tests/unit/local_tso_test.cc
)

target_include_directories(local_tso_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(local_tso_test
    GTest::GTest GTest::Main pthread
)

# ── mvcc_txn 单测（MVCC 事务层，依赖 rocksdb）────────────────────────────
add_executable(mvcc_txn_test
    tests/unit/mvcc_txn_test.cc
    src/storage/mvcc_txn.cc
    src/storage/mvcc_codec.cc
    src/storage/rocksdb_storage.cc
)
target_include_directories(mvcc_txn_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(mvcc_txn_test
    ${ROCKSDB_LIB}
    GTest::GTest GTest::Main pthread snappy z lz4
)


# ---txn-integration-test
add_executable(txn_integration_test
    tests/integration/txn_integration_test.cc
    ${PROTO_SRCS}
)
target_include_directories(txn_integration_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(txn_integration_test
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
    GTest::GTest GTest::Main
)

# ---txn-client-test（客户端事务库 Transaction + KVClient 事务接口）
add_executable(txn_client_test
    tests/integration/txn_client_test.cc
    src/client/kv_client.cc
    src/client/transaction.cc
    ${PROTO_SRCS}
)
target_include_directories(txn_client_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(txn_client_test
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
    GTest::GTest GTest::Main
)

add_executable(txn_probe
    tests/integration/txn_probe.cc
    ${PROTO_SRCS}
)
target_include_directories(txn_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(txn_probe
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
)

# ── 混沌：银行转账压测驱动（配合 tests/chaos/txn_bank_chaos_test.sh）──
add_executable(bank_chaos_test
    tests/chaos/bank_chaos_test.cc
    src/client/kv_client.cc
    src/client/transaction.cc
    ${PROTO_SRCS}
)
target_include_directories(bank_chaos_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(bank_chaos_test
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
)

# ── 混沌：离线 MVCC CF 检查器（lock CF 残锁 / write CF 悬空 start_ts）──
add_executable(mvcc_db_check
    tests/chaos/mvcc_db_check.cc
    src/storage/mvcc_codec.cc
)
target_include_directories(mvcc_db_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    /usr/local/include
)
target_link_libraries(mvcc_db_check
    ${ROCKSDB_LIB} gflags pthread snappy z lz4
)

# ── Day 2+: kv_server（源文件实现后可 make kv_server）──────────────────
add_executable(kv_server
    server/server_main.cc
    src/raft/kv_state_machine.cc
    src/service/kv_service.cc
    src/storage/mvcc_txn.cc
    src/storage/mvcc_codec.cc
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
    src/client/transaction.cc
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
    src/storage/mvcc_txn.cc
    src/storage/mvcc_codec.cc
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

# ── Day 10: optimizer_test（完美体链接修正版） ───────────────────────────────
add_executable(optimizer_test
    tests/unit/optimizer_test.cc  # 只需要留下这一个单测源文件，其余组件全部走静态库引入
)

target_include_directories(optimizer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)

target_link_libraries(optimizer_test
   sql_engine
    brpc
    ${PROTOBUF_LIBRARIES}        # 🛡️【核心修正】使用 CMake 官方探针变量，确保与主程序、bRPC 维持 100% 同源符号
    gflags
    GTest::GTest GTest::Main
    pthread
    
)


# ── Day 11: sql_executor_test ────────────────────────────────────────────
add_executable(sql_executor_test
    tests/unit/sql_executor_test.cc
)
target_include_directories(sql_executor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(sql_executor_test
    sql_engine GTest::GTest GTest::Main pthread
)

# ── Day 13: schema_manager_test ──────────────────────────────────────────
add_executable(schema_manager_test
    tests/unit/schema_manager_test.cc
)
target_include_directories(schema_manager_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(schema_manager_test
    sql_engine GTest::GTest GTest::Main pthread
)

# ── Day 14: row_codec_test ───────────────────────────────────────────────
add_executable(row_codec_test
    tests/unit/row_codec_test.cc
)
target_include_directories(row_codec_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(row_codec_test
    sql_engine GTest::GTest GTest::Main pthread
)


# ── Day 14: insert_test ──────────────────────────────────────────────────
add_executable(insert_test
    tests/unit/insert_test.cc
)
target_include_directories(insert_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(insert_test
    sql_engine GTest::GTest GTest::Main pthread
)


# ── Day 15: select_test ──────────────────────────────────────────────────
add_executable(select_test
    tests/unit/select_test.cc
)
target_include_directories(select_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(select_test
    sql_engine GTest::GTest GTest::Main pthread
)

# ── Day 16: update_delete_test ───────────────────────────────────────────
add_executable(update_delete_test
    tests/unit/update_delete_test.cc
)
target_include_directories(update_delete_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(update_delete_test
    sql_engine GTest::GTest GTest::Main pthread
)

# ── Day 16: aggregate_test ───────────────────────────────────────────────
add_executable(aggregate_test
    tests/unit/aggregate_test.cc
)
target_include_directories(aggregate_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(aggregate_test
    sql_engine GTest::GTest GTest::Main pthread
)

# ── Day 17: sql_client（交互式 SQL shell）────────────────────────────────
add_executable(sql_client
    client/sql_client_main.cc
    ${SQL_ADAPTER_SRCS}
)
target_include_directories(sql_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(sql_client
    sql_engine
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
)

# ── Day 17: sql_end_to_end_test（集成测试，需要 RaftKV 集群）────────────
add_executable(sql_end_to_end_test
    tests/integration/sql_end_to_end_test.cc
    ${SQL_ADAPTER_SRCS}
)
target_include_directories(sql_end_to_end_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    /usr/local/include
)
target_link_libraries(sql_end_to_end_test
    sql_engine
    ${BRPC_LIB}
    ${PROTOBUF_LIBRARIES} gflags pthread ssl crypto leveldb snappy z lz4
    GTest::GTest pthread
)

# ── ctest 注册 ────────────────────────────────────────────────────────────
enable_testing()
add_test(NAME storage_test       COMMAND storage_test)
add_test(NAME mvcc_codec_test    COMMAND mvcc_codec_test)
add_test(NAME state_machine_test COMMAND state_machine_test)
add_test(NAME client_test        COMMAND client_test)
add_test(NAME parser_test        COMMAND parser_test)
add_test(NAME optimizer_test     COMMAND optimizer_test)
add_test(NAME sql_executor_test  COMMAND sql_executor_test)
add_test(NAME schema_manager_test COMMAND schema_manager_test)
add_test(NAME row_codec_test     COMMAND row_codec_test)
add_test(NAME insert_test        COMMAND insert_test)
add_test(NAME select_test        COMMAND select_test)
add_test(NAME update_delete_test COMMAND update_delete_test)
add_test(NAME aggregate_test     COMMAND aggregate_test)
add_test(NAME mvcc_txn_test      COMMAND mvcc_txn_test)
add_test(NAME local_tso_test     COMMAND local_tso_test)
add_test(NAME txn_integration_test COMMAND txn_integration_test)
add_test(NAME txn_client_test     COMMAND txn_client_test)