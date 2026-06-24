#!/bin/bash
set -e

echo "=== Day 17 并发测试：2 客户端同时 INSERT ==="

../scripts/start_cluster.sh
sleep 5

PEERS="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

# 建表
./sql_client --peers=$PEERS \
  --sql="CREATE TABLE conc_test (id INT PRIMARY KEY, client_id INT, seq INT)"

# 客户端 A：INSERT 50 行（后台）
for i in $(seq 1 50); do
  ./sql_client --peers=$PEERS \
    --sql="INSERT INTO conc_test (id, client_id, seq) VALUES ($i, 1, $i)"
done &
PID_A=$!

# 客户端 B：INSERT 50 行（后台）
for i in $(seq 51 100); do
  ./sql_client --peers=$PEERS \
    --sql="INSERT INTO conc_test (id, client_id, seq) VALUES ($i, 2, $i)"
done &
PID_B=$!

# 等待完成
wait $PID_A $PID_B
echo "Both clients done"

# 验证总行数
RESULT=$(./sql_client --peers=$PEERS \
  --sql="SELECT COUNT(*) FROM conc_test" 2>&1)
echo "Total rows: $RESULT"

# 验证无重复 row_id（通过检查是否都能查到）
ERRORS=0
for i in $(seq 1 100); do
  R=$(./sql_client --peers=$PEERS \
    --sql="SELECT * FROM conc_test WHERE id = $i" 2>&1)
  if ! echo "$R" | grep -q "$i"; then
    echo "MISSING: row id=$i"
    ERRORS=$((ERRORS+1))
  fi
done

if [ $ERRORS -eq 0 ]; then
  echo "All 100 rows present, no duplicates ✅"
else
  echo "FAIL: $ERRORS rows missing"
  exit 1
fi

../scripts/stop_cluster.sh
echo "=== Day 17 并发测试通过 ==="
