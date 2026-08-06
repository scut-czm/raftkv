#include "braft/configuration.h"
#include "raft/kv_state_machine.h"
#include "service/kv_service.h"
#include "storage/rocksdb_storage.h"

#include <braft/raft.h>
#include <brpc/server.h>
#include <butil/logging.h>
#include <filesystem>
#include <gflags/gflags.h>
#include <memory>

DEFINE_int32(port, 8200, "节点监听端口");
DEFINE_string(ip, "127.0.0.1", "节点 IP 地址");
DEFINE_string(group, "RaftKVGroup", "Raft 组名");
DEFINE_string(conf, "", "初始集群配置 (IP1:Port1:0,IP2:Port2:0,...)");
DEFINE_string(data_path, "./data", "数据存储根路径");
DEFINE_int32(election_timeout_ms, 3000, "选举超时（毫秒）");
DEFINE_int32(snapshot_interval_s, 3600, "快照间隔（秒），调试期间可设小");

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  LOG(INFO) << "启动 RaftKV 节点 " << FLAGS_ip << ":" << FLAGS_port
            << "  group=" << FLAGS_group << "  data=" << FLAGS_data_path;

  // 1. 初始化 RocksDB 存储
  // RocksDB 关闭了 WAL（持久性由 braft 日志保证），崩溃后残留的 DB
  // 只是「掉了 memtable、且各 CF 刷盘进度不一」的脏状态，直接在其上
  // 重放日志会得到与其他副本不同的结果（如 Prewrite 冲突判定翻转），
  // 造成副本发散。因此启动时删掉旧 DB，从 braft snapshot + 日志重建。
  raftkv::StorageOptions storage_opts;
  storage_opts.db_path = FLAGS_data_path + "/rocksdb";
  std::error_code ec;
  std::filesystem::remove_all(storage_opts.db_path, ec);
  if (ec) {
    LOG(ERROR) << "清理残留 RocksDB 失败: " << storage_opts.db_path << " "
               << ec.message();
    return -1;
  }
  auto storage = std::make_shared<raftkv::RocksDbStorage>(storage_opts);
  if (!storage->Open()) {
    LOG(ERROR) << "RocksDB 打开失败: " << storage_opts.db_path;
    return -1;
  }
  LOG(INFO) << "RocksDB 已打开: " << storage_opts.db_path;

  // 2. 创建 brpc Server，注册 braft 内部服务
  brpc::Server server;
  if (braft::add_service(&server, FLAGS_port) != 0) {
    LOG(ERROR) << "添加 braft 服务失败";
    return -1;
  }
  // 3. 创建状态机（注入 RocksDB）
  raftkv::KVStateMachine fsm(storage);

  // 4. 配置 Raft 节点
  braft::NodeOptions node_opts;
  node_opts.fsm = &fsm;
  node_opts.log_uri = "local://" + FLAGS_data_path + "/log";
  node_opts.snapshot_uri = "local://" + FLAGS_data_path + "/snapshot";
  node_opts.raft_meta_uri = "local://" + FLAGS_data_path + "/meta";
  node_opts.election_timeout_ms = FLAGS_election_timeout_ms;
  node_opts.snapshot_interval_s = FLAGS_snapshot_interval_s;

  if (!FLAGS_conf.empty() &&
      node_opts.initial_conf.parse_from(FLAGS_conf) != 0) {
    LOG(ERROR) << "解析集群配置失败: " << FLAGS_conf;
    return -1;
  }
  // 5. 构造本节点 PeerId
  braft::PeerId peer_id;
  std::string peer_addr = FLAGS_ip + ":" + std::to_string(FLAGS_port) + ":0";
  if (peer_id.parse(peer_addr) != 0) {
    LOG(ERROR) << "解析 PeerId 失败: " << peer_addr;
    return -1;
  }
  // 6. 初始化 Raft 节点
  auto *node = new braft::Node(FLAGS_group, peer_id);
  if (node->init(node_opts) != 0) {
    LOG(ERROR) << "初始化 Raft 节点失败";
    return -1;
  }
  LOG(INFO) << "Raft 节点已初始化: " << FLAGS_group << " "
            << peer_id.to_string();
  // 7. 注册 KV RPC 服务
  raftkv::KVServiceImpl kv_service(node, &fsm);
  if (server.AddService(&kv_service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    LOG(ERROR) << "注册 KV 服务失败";
    return -1;
  }
  // 8. 启动 Server
  if (server.Start(FLAGS_port, nullptr) != 0) {
    LOG(ERROR) << "Server 启动失败";
    return -1;
  }
  LOG(INFO) << "Server 启动成功，端口: " << FLAGS_port;

  server.RunUntilAskedToQuit();

  LOG(INFO) << "正在关闭...";
  node->shutdown(nullptr);
  node->join();
  delete node;
  return 0;
}