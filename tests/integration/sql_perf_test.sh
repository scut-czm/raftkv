#!/bin/bash
echo "=== SQL 性能基准 ==="

PEERS="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

# 建表
./sql_client --peers=$PEERS \
  --sql="CREATE TABLE perf_t (id INT PRIMARY KEY, val VARCHAR(64), num INT)"

# INSERT TPS 测试
echo "[INSERT TPS]"
START=$(date +%s%N)
for i in $(seq 1 1000); do
  ./sql_client --peers=$PEERS \
    --sql="INSERT INTO perf_t (id, val, num) VALUES ($i, 'val_$i', $i)" \
    2>/dev/null
done
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
TPS=$(( 1000 * 1000 / ELAPSED_MS ))
echo "  1000 INSERTs in ${ELAPSED_MS}ms → TPS ≈ $TPS"

# SELECT 延迟测试
echo "[SELECT latency]"
START=$(date +%s%N)
for i in $(seq 1 100); do
  ./sql_client --peers=$PEERS \
    --sql="SELECT * FROM perf_t WHERE id = $((RANDOM % 1000 + 1))" \
    2>/dev/null
done
END=$(date +%s%N)
AVG_US=$(( (END - START) / 100000 ))
echo "  100 point SELECTs, avg latency ≈ ${AVG_US}us"

# SELECT 全表扫描
echo "[SELECT full scan]"
START=$(date +%s%N)
./sql_client --peers=$PEERS \
  --sql="SELECT COUNT(*) FROM perf_t" 2>/dev/null
END=$(date +%s%N)
ELAPSED_US=$(( (END - START) / 1000 ))
echo "  COUNT(*) 1000 rows: ${ELAPSED_US}us"

echo "=== 性能基准完成 ==="