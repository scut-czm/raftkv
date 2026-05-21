// 集成测试通过 shell 脚本驱动：
//   bash tests/integration/run_integration.sh
//
// 测试场景：
//   1. [P0] 3节点写入100条，三节点数据一致
//   2. [P0] Kill Follower → 写入不受影响 → 恢复后数据同步
//   3. [P1] Kill Leader → 重新选举 → 写入恢复
//   4. [P2] Scan redirect 正确
//   5. [P1] Put → Delete → Get 验证删除
//   6. [P0] 线性一致读验证

#include <iostream>

int main() {
  std::cout << "集成测试请运行: bash tests/integration/run_integration.sh"
            << std::endl;
  return 0;
}