// TODO: gflags 定义 --peers / --timeout_ms
// TODO: PrintResult() —— 打印列头 + 分隔线 + 行数据 / OK N rows affected
// TODO: main() —— KVClient(opts) + RaftKvClientAdapter + SQLExecutor
// TODO: 交互式循环 —— 读行、拼接 sql_buf，遇 ';' 时执行，quit/exit 退出

#include <gflags/gflags.h>

#include "src/client/kv_client.h"
#include "src/sql/raft_kv_client_adapter.h"
#include "src/sql/sql_executor.h"
#include <iomanip>
#include <iostream>
#include <string>

#if 0
// 注册 GFlags 命令行核心标志
DEFINE_string(peers, "127.0.0.1:8200",
              "集群节点列表（逗号分隔），如 127.0.0.1:8200,127.0.0.1:8201");
DEFINE_string(
    sql, "",
    "要单笔执行的 SQL 语句（若为空则直接自动切入 Interactive 交互式终端）");
DEFINE_int32(timeout_ms, 3000, "RPC 跨网络通信超时（毫秒）");

// =========================================================================
// 优化点 1：格式化表格输出（完美对齐弹性防御版）
// =========================================================================
void PrintTable(const raftsql::SQLExecutor::QueryResult &result) {
  if (result.rows.empty()) {
    std::cout << "Empty set" << std::endl;
    return;
  }

  // 1. 收集所有列名并执行严格的 ASCII 字典序重排，确保展现顺序完全恒定
  std::vector<std::string> columns;
  for (const auto &[col, _] : result.rows[0]) {
    columns.push_back(col);
  }
  std::sort(columns.begin(), columns.end());

  // 2. 动态计算最高增益列宽账本
  std::vector<size_t> widths(columns.size());
  for (size_t i = 0; i < columns.size(); ++i) {
    widths[i] = columns[i].size();
    for (const auto &row : result.rows) {
      auto it = row.find(columns[i]);
      // 【终极物理修复】如果字段缺失（即 SQL 语义下的标准 NULL），
      // 参与对比的实际文本长度必须强行计为 "NULL" 的长度（4），彻底杜绝边框错位
      size_t actual_len = (it != row.end()) ? it->second.size() : 4;
      widths[i] = std::max(widths[i], actual_len);
    }
  }

  // 3. 闭包：动态渲染 ASCII 分隔横线
  auto print_separator = [&]() {
    std::cout << "+";
    for (size_t w : widths) {
      std::cout << std::string(w + 2, '-')
                << "+"; // 各留 2 个空格作为 Padding 填衬
    }
    std::cout << "\n";
  };

  // 4. 泵出表头
  print_separator();
  std::cout << "|";
  for (size_t i = 0; i < columns.size(); ++i) {
    std::cout << " " << std::left << std::setw(widths[i]) << columns[i] << " |";
  }
  std::cout << "\n";
  print_separator();

  // 5. 流式流泵出所有合规行数据
  for (const auto &row : result.rows) {
    std::cout << "|";
    for (size_t i = 0; i < columns.size(); ++i) {
      auto it = row.find(columns[i]);
      std::string val = (it != row.end()) ? it->second : "NULL";
      // 依靠 std::left 强制靠左，std::setw 锁定特定物理宽度槽位
      std::cout << " " << std::left << std::setw(widths[i]) << val << " |";
    }
    std::cout << "\n";
  }
  print_separator();
  std::cout << result.rows.size() << " row(s) in set" << std::endl;
}

// =========================================================================
// 优化点 2：REPL 交互式终端外壳引擎（O(1) 极速空间清洗版）
// =========================================================================

void RunInteractive(raftsql::SQLExecutor *executor) {
  std::string line;
  std::cout << "RaftSQL> " << std::flush;

  while (std::getline(std::cin, line)) {
    // 【终极物理修复】利用双向嗅探算法，达成 O(1) 复杂度的 0 内存挪动 Trim 优化
    size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos) {
      line.clear(); // 全是空格，一洗而空
    } else {
      size_t last = line.find_last_not_of(' ');
      line = line.substr(first, last - first + 1);
    }
    if (line.empty()) {
      std::cout << "RaftSQL> " << std::flush;
      continue;
    }

    // 内置元命令强行捕获拦截
    if (line == "\\q" || line == "quit" || line == "exit") {
      break;
    }
    if (line == "\\tables" || line == "SHOW TABLES") {
      std::cout << "(SHOW TABLES not yet implemented via shell, coming soon)"
                << std::endl;
      std::cout << "RaftSQL> " << std::flush;
      continue;
    }
    // 点火打通 SQL 执行管道
    auto result = executor->Execute(line);
    if (!result.ok) {
      std::cerr << "ERROR: " << result.error_msg << std::endl;
    } else if (!result.rows.empty()) {
      PrintTable(result); // DQL 路径：流式高保真打印
    } else if (result.affected_rows > 0) {
      std::cout << "Query OK, " << result.affected_rows << " row(s) affected"
                << std::endl;
    } else {
      std::cout << "Query OK" << std::endl; // DDL 路径保底
    }

    std::cout << "RaftSQL> " << std::flush;
  }
  std::cout << "Bye!" << std::endl;
}

// =========================================================================
// 程序主入口：智能分流单笔执行与多模交互状态机
// =========================================================================

int main(int argc,char *argv[]){
  gflags::ParseCommandLineFlags(&argc,&argv,true);

  // 1. 激活大后方真实物理分布式通信网络客户端
  raftkv::ClientOptions opts;
  opts.peers=FLAGS_peers;
  opts.timeout_ms=FLAGS_timeout_ms;
  opts.linearizable=true;

  raftkv::KVClient kv_client(opts);

  // 2. 注入连接适配器，复活 SQL 大内总管
  raftsql::SQLExecutor executor(&kv_client);
  

}
#endif

#if 0
DEFINE_string(peers, "127.0.0.1:8200", "RaftKV cluster peers, comma-separated");
DEFINE_int32(timeout_ms, 3000, "RPC timeout in milliseconds");

static void PrintResult(const raftsql::SQLExecutor::QueryResult &result) {
  if (!result.ok) {
    std::cerr << "ERROR: " << result.error_msg << "\n";
    return;
  }

  if (!result.rows.empty()) {
    // 打印列头（从第一行取 key）
    bool header_printed = false;
    for (const auto &row : result.rows) {
      if (!header_printed) {
        bool first = true;
        for (const auto &[col, _] : row) {
          if (!first)
            std::cout << " | ";
          std::cout << col;
          first = false;
        }
        std::cout << "\n";
        for (size_t i = 0; i < 40; ++i)
          std::cout << "-";
        std::cout << "\n";
        header_printed = true;
      }
      bool first = true;
      for (const auto &[col, val] : row) {
        if (!first)
          std::cout << " | ";
        std::cout << val;
        first = false;
      }
      std::cout << "\n";
    }
    std::cout << "(" << result.rows.size() << " row"
              << (result.rows.size() == 1 ? "" : "s") << ")\n";
  } else if (result.affected_rows > 0) {
    std::cout << "OK (" << result.affected_rows << " row"
              << (result.affected_rows == 1 ? "" : "s") << " affected)\n";
  } else {
    std::cout << "OK\n";
  }
}

int main(int argc, char *argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  raftkv::ClientOptions opts;
  opts.peers = FLAGS_peers;
  opts.timeout_ms = FLAGS_timeout_ms;

  raftkv::KVClient kv_client(opts);
  raftsql::RaftKvClientAdapter adapter(&kv_client);
  raftsql::SQLExecutor executor(&adapter);

  std::cout << "RaftSQL v1.0  (connected to " << FLAGS_peers << ")\n";
  std::cout << "Type 'quit' or 'exit' to leave.\n\n";

  std::string line;
  std::string sql_buf;

  while (true) {
    std::cout << (sql_buf.empty() ? "raftsql> " : "      -> ");
    if (!std::getline(std::cin, line)) {
      break;
    }

    //// 去除首尾空白
    size_t start = line.find_first_not_of(" \t\r\n");
    size_t end = line.find_last_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    line = line.substr(start, end - start + 1);
    if (line == "quit" || line == "exit") {
      break;
    }

    sql_buf += (sql_buf.empty() ? "" : " ") + line;

    // 以 ; 结尾时执行
    if (!sql_buf.empty() && sql_buf.back() == ';') {
      sql_buf.pop_back(); // 去掉分号
      auto result = executor.Execute(sql_buf);
      PrintResult(result);
      sql_buf.clear();
    }
  }
  std::cout << "Bye.\n";
  return 0;
}
#endif

#include <algorithm>
#include <gflags/gflags.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "src/client/kv_client.h"
#include "src/sql/raft_kv_client_adapter.h"
#include "src/sql/sql_executor.h"

// ── [核心物化 1] 注册全套 GFlags 命令行标志 ───────────────────────────────
DEFINE_string(peers, "127.0.0.1:8200", "RaftKV cluster peers, comma-separated");
DEFINE_int32(timeout_ms, 3000, "RPC timeout in milliseconds");
// 彻底消灭 unknown command line flag 'sql' 报错的终极布防
DEFINE_string(sql, "",
              "SQL statement to execute. If empty, enter interactive shell.");

// ── [优化点 1] 格式化表格输出（自动防错位与 NULL 审计版） ──────────────────
static void PrintResult(const raftsql::SQLExecutor::QueryResult &result,
                        const std::string &raw_sql) {
  if (!result.ok) {
    std::cerr << "ERROR: " << result.error_msg << "\n";
    return;
  }

  if (!result.rows.empty()) {
    // 1. 动态收集并强制执行严格的 ASCII 字典序重排，确保列展现顺序恒定
    std::vector<std::string> columns;
    for (const auto &[col, _] : result.rows[0]) {
      columns.push_back(col);
    }
    std::sort(columns.begin(), columns.end());

    // 2. 动态计算最高增益列宽账本
    std::vector<size_t> widths(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
      widths[i] = columns[i].size();
      for (const auto &row : result.rows) {
        auto it = row.find(columns[i]);
        // 稀疏矩阵刚性防御：如果字段缺失（NULL），长度强行计为
        // 4，彻底根治锯齿边框错位
        size_t actual_len = (it != row.end()) ? it->second.size() : 4;
        widths[i] = std::max(widths[i], actual_len);
      }
    }

    // 3. 渲染 ASCII 完美闭合横线
    auto print_separator = [&]() {
      std::cout << "+";
      for (size_t w : widths) {
        std::cout << std::string(w + 2, '-') << "+";
      }
      std::cout << "\n";
    };

    // 4. 泵出对齐表头
    print_separator();
    std::cout << "|";
    for (size_t i = 0; i < columns.size(); ++i) {
      std::cout << " " << std::left << std::setw(widths[i]) << columns[i]
                << " |";
    }
    std::cout << "\n";
    print_separator();

    // 5. 流式泵出实体行数据
    for (const auto &row : result.rows) {
      std::cout << "|";
      for (size_t i = 0; i < columns.size(); ++i) {
        auto it = row.find(columns[i]);
        std::string val = (it != row.end()) ? it->second : "NULL";
        std::cout << " " << std::left << std::setw(widths[i]) << val << " |";
      }
      std::cout << "\n";
    }
    print_separator();
    std::cout << result.rows.size() << " row"
              << (result.rows.size() == 1 ? "" : "s") << " in set\n";

  } else if (result.affected_rows > 0) {
    std::cout << "Query OK, " << result.affected_rows << " row"
              << (result.affected_rows == 1 ? "" : "s") << " affected\n";
  } else {
    // std::cout << "Query OK\n";
    // 【核心修正】提取 SQL 头部关键字进行大小写不敏感匹配，精准拦截空集合

    std::string norm_sql = raw_sql;
    std::transform(norm_sql.begin(), norm_sql.end(), norm_sql.begin(),
                   ::toupper);
    // 移除潜在的左侧残存空白（虽然外层清洗过，此处做刚性防御）
    size_t first_char = norm_sql.find_first_not_of(" \t\r\n");
    if (first_char != std::string::npos) {
      norm_sql = norm_sql.substr(first_char);
    }

    if (norm_sql.rfind("SELECT", 0) == 0 || norm_sql.rfind("SHOW", 0) == 0) {
      std::cout << "Empty set (0 rows)\n"; // 完美达成 MySQL 级体验对齐
    } else {
      std::cout << "Query OK\n"; // 真正的 DDL（如 CREATE TABLE）保底路径
    }
  }
}

// ── [优化点 2] REPL 多行换行缓冲 Shell 引擎（O(1) 空间清洗） ─────────────────
void RunInteractive(raftsql::SQLExecutor *executor) {
  std::cout << "Welcome to RaftSQL Cluster Shell Terminal.\n";
  std::cout << "Cluster Peers connected: " << FLAGS_peers << "\n";
  std::cout
      << "Type 'quit' or 'exit' to leave. Use ';' to execute queries.\n\n";

  std::string line;
  std::string sql_buf;

  while (true) {
    // 智能动态提示符：缓冲池为空提示新请求，包含未完结的换行时提示追加箭头 ->
    std::cout << (sql_buf.empty() ? "raftsql> " : "    -> ") << std::flush;

    if (!std::getline(std::cin, line)) {
      break;
    }

    // 极速双向嗅探算法，达成 O(1) 复杂度的 0 内存挪动 Trim 空间清洗
    size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      continue; // 全是危险空格，直接拦截空转
    }
    size_t last = line.find_last_not_of(" \t\r\n");
    line = line.substr(first, last - first + 1);

    // 快捷元命令常驻捕获
    if (line == "quit" || line == "exit") {
      break;
    }

    // 将本行洗干净的内容流式追加进 SQL 换行缓冲区
    sql_buf += (sql_buf.empty() ? "" : " ") + line;

    // 关键判点：当且仅当缓冲区尾部撞见分号时，高压执行状态机才全面通电触发！
    if (!sql_buf.empty() && sql_buf.back() == ';') {
      sql_buf.pop_back(); // 剥离掉起修饰作用的末尾分号

      auto result = executor->Execute(sql_buf);
      PrintResult(result, sql_buf);

      sql_buf.clear(); // 干净利落地清空大坝，准备迎接下一波多行 SQL
    }
  }
  std::cout << "Bye.\n";
}

// ── [核心物化 2] 智能分流主入口 ──────────────────────────────────────────
int main(int argc, char *argv[]) {
  // 解析 GFlags 命令行拓扑
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // 1. 激活存储层网络客户端与线性一致性配置
  raftkv::ClientOptions opts;
  opts.peers = FLAGS_peers;
  opts.timeout_ms = FLAGS_timeout_ms;
  opts.linearizable = true; // 确保集成故障压测时，读写始终维持线性一致强防御
  raftkv::KVClient kv_client(opts);

  // 2. 桥接多态适配器，注入 SQL 执行核心
  raftsql::RaftKvClientAdapter adapter(&kv_client);
  raftsql::SQLExecutor executor(&adapter);

  // 3. 智能分流状态机：完美兼顾脚本自动化批处理与人类手工 REPL 交互
  if (!FLAGS_sql.empty()) {
    // 模态 A：非交互式快路径（专门用来承接 sql_failover_test.sh
    // 等自动化集成测试脚本）
    auto result = executor.Execute(FLAGS_sql);
    PrintResult(result, FLAGS_sql);

    // 如果执行失败，返回非 0 状态码，促使上层集成测试脚本感知断线
    return result.ok ? 0 : -1;
  } else {
    // 模态 B：回归经典多行交互式优雅外壳
    RunInteractive(&executor);
  }

  return 0;
}
