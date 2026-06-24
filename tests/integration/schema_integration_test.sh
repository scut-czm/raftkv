#!/bin/bash
set -e

echo "=== Day 13 集成测试：Schema 管理 ==="

# 启动 3 节点集群
../scripts/start_cluster.sh
sleep 5

PEERS="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

# 1. CREATE TABLE（在 Leader 上执行）
echo "[Test 1] CREATE TABLE"
./sql_client --peers=$PEERS \
  --sql="CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT)"
echo "CREATE TABLE: OK"

# 2. 从所有节点验证 Schema 存在
echo "[Test 2] 验证 3 节点 Schema 一致"
for PORT in 8200 8201 8202; do
  RESULT=$(./kv_client --peers=127.0.0.1:$PORT \
    --command=get --key="__schema__/users" 2>/dev/null)
  if [ -z "$RESULT" ]; then
    echo "FAIL: Node $PORT 无 Schema"
    exit 1
  fi
  echo "  Node $PORT: Schema 存在 ✅"
done

# 3. 重复 CREATE → 应报错
echo "[Test 3] 重复 CREATE TABLE"
RESULT=$(./sql_client --peers=$PEERS \
  --sql="CREATE TABLE users (id INT PRIMARY KEY)" 2>&1 || true)
if echo "$RESULT" | grep -q "already exists"; then
  echo "  重复建表正确返回错误 ✅"
else
  echo "FAIL: 重复建表未报错"
  exit 1
fi

# 4. SHOW TABLES（ListTables）
echo "[Test 4] LIST TABLES"
./sql_client --peers=$PEERS --sql="SHOW TABLES"

# 清理
../scripts/stop_cluster.sh
echo "=== Day 13 集成测试全部通过 ==="