#include "client/kv_client.h"

#include <gflags/gflags.h>
#include <iostream>
#include <string>
#include <vector>

DEFINE_string(peers, "127.0.0.1:8200",
              "集群节点列表（逗号分隔），如 127.0.0.1:8200,127.0.0.1:8201");
DEFINE_string(command, "put", "命令类型 (put/get/delete/scan)");
DEFINE_string(key, "test_key", "键（put/get/delete 使用）");
DEFINE_string(value, "test_value", "值（put 使用）");
DEFINE_string(start_key, "", "扫描起始 key（scan 使用，含）");
DEFINE_string(end_key, "", "扫描结束 key（scan 使用，不含）");
DEFINE_int32(limit, 10, "scan 最多返回条数，0 表示不限制");
DEFINE_bool(linearizable, true, "是否使用线性一致读（Get/Scan）");
DEFINE_int32(timeout_ms, 3000, "RPC 超时（毫秒）");

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  raftkv::ClientOptions opts;
  opts.peers = FLAGS_peers;
  opts.linearizable = FLAGS_linearizable;
  opts.timeout_ms = FLAGS_timeout_ms;
  raftkv::KVClient client(opts);

  std::cout << "[peers=" << FLAGS_peers
            << ", linearizable=" << FLAGS_linearizable
            << "] command=" << FLAGS_command << std::endl;

  if (FLAGS_command == "put") {
    bool ok = client.Put(FLAGS_key, FLAGS_value);
    std::cout << (ok ? "PUT OK" : "PUT FAILED") << "  key=" << FLAGS_key
              << "  value=" << FLAGS_value << std::endl;

  } else if (FLAGS_command == "get") {
    std::string value;
    bool found = false;
    bool ok = client.Get(FLAGS_key, &value, &found);
    if (ok) {
      if (found) {
        std::cout << "GET  key=" << FLAGS_key << "  value=" << value
                  << std::endl;
      } else {
        std::cout << "GET  key=" << FLAGS_key << "  (not found)" << std::endl;
      }
    } else {
      std::cout << "GET FAILED  key=" << FLAGS_key << std::endl;
    }

  } else if (FLAGS_command == "delete") {
    bool ok = client.Delete(FLAGS_key);
    std::cout << (ok ? "DELETE OK" : "DELETE FAILED") << "  key=" << FLAGS_key
              << std::endl;

  } else if (FLAGS_command == "scan") {
    std::vector<std::pair<std::string, std::string>> kvs;
    bool ok = client.Scan(FLAGS_start_key, FLAGS_end_key, FLAGS_limit, &kvs);
    if (ok) {
      std::cout << "SCAN  start=" << FLAGS_start_key
                << "  end=" << FLAGS_end_key << "  limit=" << FLAGS_limit
                << "  got=" << kvs.size() << std::endl;
      for (const auto &[k, v] : kvs) {
        std::cout << "  " << k << " = " << v << std::endl;
      }
    } else {
      std::cout << "SCAN FAILED" << std::endl;
    }

  } else {
    std::cout << "未知命令: " << FLAGS_command << std::endl;
    std::cout << "支持: put / get / delete / scan" << std::endl;
    return -1;
  }

  return 0;
}
