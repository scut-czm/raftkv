#!/usr/bin/env bash
# 多轮测试并汇总结果
# 用法: ./bench_compare.sh <label> <rounds>

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
PEERS="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"
LABEL="${1:-test}"
ROUNDS="${2:-3}"

echo "========================================"
echo "  标签: $LABEL"
echo "  轮次: $ROUNDS"
echo "========================================"

tps_list=()
p99_list=()

for i in $(seq 1 "$ROUNDS"); do
  echo ""
  echo "── Round $i/$ROUNDS ──"

  "$SCRIPT_DIR/stop_cluster.sh" > /dev/null 2>&1 || true
  sleep 1
  "$SCRIPT_DIR/start_cluster.sh" > /dev/null 2>&1
  sleep 5

  # 确认 Leader 已选出
  for attempt in $(seq 1 10); do
    if grep -q 'Became LEADER' /tmp/raftkv_820*.log 2>/dev/null; then
      break
    fi
    sleep 1
  done

  result=$("$BUILD_DIR/perf_test" \
    --peers="$PEERS" \
    --mode=write \
    --threads=32 \
    --duration_s=60 \
    --value_size=1024 \
    --write_ratio=1.0 2>&1)

  tps=$(echo "$result" | grep "TPS:" | tail -1 | awk '{print $2}')
  p99=$(echo "$result" | grep "\[Write\]" | grep -oP 'p99=\K[0-9.]+')
  status=$(echo "$result" | grep -oP '>>>[^<]+<<<' | head -1)

  echo "  TPS=$tps  p99=${p99}ms  $status"
  tps_list+=("$tps")
  p99_list+=("$p99")
done

"$SCRIPT_DIR/stop_cluster.sh" > /dev/null 2>&1 || true

echo ""
echo "========================================"
echo "  [$LABEL] 汇总结果"
echo "========================================"
for i in "${!tps_list[@]}"; do
  echo "  Round $((i+1)): TPS=${tps_list[$i]}  p99=${p99_list[$i]}ms"
done

# 计算平均 TPS
avg_tps=$(echo "${tps_list[@]}" | tr ' ' '\n' | \
  awk '{s+=$1; n++} END {printf "%.0f", s/n}')
avg_p99=$(echo "${p99_list[@]}" | tr ' ' '\n' | \
  awk '{s+=$1; n++} END {printf "%.2f", s/n}')

echo "  ─────────────────────────────────────"
echo "  平均 TPS: $avg_tps"
echo "  平均 p99: ${avg_p99}ms"
if [ "$avg_tps" -ge 15000 ] 2>/dev/null; then
  echo "  >>> PASS: 平均 TPS >= 15K <<<"
else
  echo "  >>> WARN: 平均 TPS 未达 15K <<<"
fi
echo "========================================"
