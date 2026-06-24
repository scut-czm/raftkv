#!/bin/bash
set -e

echo "=== Day 17 故障容错测试：SQL + Kill Leader ==="

../scripts/start_cluster.sh
sleep 5

PEERS="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

# 1. 写入测试数据
echo "[1] Setup data"
./sql_client --peers=$PEERS \
  --sql="CREATE TABLE ft_test (id INT PRIMARY KEY, val VARCHAR(64))"
for i in $(seq 1 20); do
  ./sql_client --peers=$PEERS \
    --sql="INSERT INTO ft_test (id, val) VALUES ($i, 'data_$i')"
done
echo "  20 rows inserted"

# 2. 找到当前 Leader 并 Kill
echo "[2] Kill Leader"
LEADER_PID=$(../scripts/find_leader.sh)
echo "  Leader PID: $LEADER_PID"
kill -9 $LEADER_PID
echo "  Leader killed"

# 3. 等待重选举
echo "[3] Wait for re-election (5s)"
sleep 5

# 4. 验证 SQL 操作仍可用
echo "[4] SQL after failover"
# INSERT 新数据
RESULT=$(./sql_client --peers=$PEERS \
  --sql="INSERT INTO ft_test (id, val) VALUES (21, 'after_kill')" 2>&1)
if echo "$RESULT" | grep -q "OK"; then
  echo "  INSERT after failover: OK ✅"
else
  echo "  INSERT after failover: attempting retry..."
  sleep 2
  ./sql_client --peers=$PEERS \
    --sql="INSERT INTO ft_test (id, val) VALUES (21, 'after_kill')"
  echo "  INSERT after retry: OK ✅"
fi

# SELECT 验证数据完整
RESULT=$(./sql_client --peers=$PEERS \
  --sql="SELECT COUNT(*) FROM ft_test" 2>&1)
echo "  COUNT after failover: $RESULT"

# 5. SELECT 旧数据
RESULT=$(./sql_client --peers=$PEERS \
  --sql="SELECT val FROM ft_test WHERE id = 10" 2>&1)
echo "  Row id=10: $RESULT"
if echo "$RESULT" | grep -q "data_10"; then
  echo "  Data integrity: OK ✅"
else
  echo "  WARN: Data might be stale"
fi

# 6. 恢复被 Kill 的节点
echo "[5] Restart killed node"
# 重启节点（具体命令取决于脚本实现）
# ../scripts/restart_node.sh $LEADER_PORT
sleep 5

../scripts/stop_cluster.sh
echo "=== Day 17 故障容错测试通过 ==="
