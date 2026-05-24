#!/usr/bin/env bash
# 读写混合测试：RocksDB 优化 vs 基线 对比
# 用法: ./bench_rw_compare.sh [rounds]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
SRC_CC="$SCRIPT_DIR/../src/storage/rocksdb_storage.cc"
PEERS="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"
ROUNDS="${1:-3}"
THREADS=8      # readwrite 受服务端 bthread_concurrency(默认=9) 限制，用 8 线程
DURATION=60
VALUE_SIZE=1024
WRITE_RATIO=0.5

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'
BOLD='\033[1m'; NC='\033[0m'

log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()   { echo -e "${GREEN}[PASS]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

prefill() {
  log "预填充 ~60s 数据（写入 ~480MB/节点，触发多轮 SST flush，激活 Bloom Filter / Block Cache）..."
  "$BUILD_DIR/perf_test" \
    --peers="$PEERS" --mode=write --threads=16 \
    --duration_s=60 --value_size="$VALUE_SIZE" --write_ratio=1.0 \
    > /dev/null 2>&1 || true
}

start_cluster() {
  "$SCRIPT_DIR/stop_cluster.sh" > /dev/null 2>&1 || true
  sleep 1
  "$SCRIPT_DIR/start_cluster.sh" > /dev/null 2>&1
  for attempt in $(seq 1 15); do
    grep -q 'Became LEADER' /tmp/raftkv_820*.log 2>/dev/null && break || true
    sleep 1
  done
}

rebuild() {
  log "编译中..."
  make -j"$(nproc)" -C "$BUILD_DIR" 2>&1 | grep -E "Built target|error:" || true
}

run_bench() {
  local label="$1"
  local tps_sum=0 p99w_sum=0 p99r_sum=0
  declare -a tps_list p99w_list p99r_list

  echo ""
  echo -e "${BOLD}════════════════════════════════════════${NC}"
  echo -e "${BOLD}  配置: $label${NC}"
  echo -e "${BOLD}════════════════════════════════════════${NC}"

  for i in $(seq 1 "$ROUNDS"); do
    log "── Round $i/$ROUNDS ──"
    start_cluster
    prefill

    result=$("$BUILD_DIR/perf_test" \
      --peers="$PEERS" --mode=readwrite \
      --threads="$THREADS" --duration_s="$DURATION" \
      --value_size="$VALUE_SIZE" --write_ratio="$WRITE_RATIO" 2>&1)

    tps=$(echo "$result"  | grep "^  TPS:"   | awk '{print $2}' || echo "0")
    p99w=$(echo "$result" | grep "\[Write\]" | grep -oP 'p99=\K[0-9.]+' || echo "0")
    p99r=$(echo "$result" | grep "\[Read"    | grep -oP 'p99=\K[0-9.]+' || echo "0")
    status=$(echo "$result" | grep -oP '>>>[^<]+<<<' | head -1 || echo "")

    echo "  TPS=$tps  Write-p99=${p99w}ms  Read-p99=${p99r}ms  $status"
    tps_list+=("$tps")
    p99w_list+=("$p99w")
    p99r_list+=("$p99r")
  done

  "$SCRIPT_DIR/stop_cluster.sh" > /dev/null 2>&1 || true

  local avg_tps avg_p99w avg_p99r
  avg_tps=$(printf '%s\n'  "${tps_list[@]}"  | awk '{s+=$1;n++}END{printf "%.0f",s/n}' || echo "0")
  avg_p99w=$(printf '%s\n' "${p99w_list[@]}" | awk '{s+=$1;n++}END{printf "%.2f",s/n}' || echo "0")
  avg_p99r=$(printf '%s\n' "${p99r_list[@]}" | awk '{s+=$1;n++}END{printf "%.2f",s/n}' || echo "0")

  echo "  ────────────────────────────────────────"
  echo "  平均 TPS=$avg_tps  Write-p99=${avg_p99w}ms  Read-p99=${avg_p99r}ms"

  # 导出供汇总使用
  RESULT_LABEL="$label"
  RESULT_TPS="$avg_tps"
  RESULT_P99W="$avg_p99w"
  RESULT_P99R="$avg_p99r"
  RESULT_TPS_LIST=("${tps_list[@]}")
  RESULT_P99W_LIST=("${p99w_list[@]}")
  RESULT_P99R_LIST=("${p99r_list[@]}")
}

# ── 切换到基线配置 ────────────────────────────────────
apply_baseline() {
  log "备份并切换到 RocksDB 基线配置..."
  cp "$SRC_CC" "${SRC_CC}.optimized.bak"

  python3 - "$SRC_CC" <<'PYEOF'
import re, sys
path = sys.argv[1]
with open(path) as f:
    src = f.read()

# 构造函数：去掉 disableWAL 设置
src = re.sub(
    r'(RocksDbStorage::RocksDbStorage\(StorageOptions options\)\s*'
    r': options_\(std::move\(options\)\)\) \{)[^}]*(})',
    r'\1\n  // BASELINE\n\2',
    src, flags=re.DOTALL
)

# Open()：把 Block Cache 到 stop_writes_trigger 整块替换为最简配置
src = re.sub(
    r'(db_opts\.max_background_jobs = options_\.max_background_jobs;\n)'
    r'.*?'
    r'(data_cf_opts\.level0_stop_writes_trigger = 80;\n)',
    r'''\1
  rocksdb::ColumnFamilyOptions data_cf_opts;
  data_cf_opts.write_buffer_size = options_.write_buffer_size;
  data_cf_opts.max_write_buffer_number = options_.max_write_buffer_number;
''',
    src, flags=re.DOTALL
)

with open(path, 'w') as f:
    f.write(src)
print("  基线配置已写入")
PYEOF
}

restore_optimized() {
  if [ -f "${SRC_CC}.optimized.bak" ]; then
    log "恢复 RocksDB 优化配置..."
    cp "${SRC_CC}.optimized.bak" "$SRC_CC"
    rm "${SRC_CC}.optimized.bak"
  fi
}

# ─────────────────────────────────────────────────────
#  主流程
# ─────────────────────────────────────────────────────
echo -e "${BOLD}"
echo "  RaftKV 读写混合性能对比  (write_ratio=${WRITE_RATIO}, threads=${THREADS})"
echo -e "${NC}"

# ① 优化配置测试
run_bench "RocksDB 已优化（disableWAL + BlockCache + BloomFilter + LZ4）"
OPT_TPS="$RESULT_TPS"; OPT_P99W="$RESULT_P99W"; OPT_P99R="$RESULT_P99R"
OPT_TPS_LIST=("${RESULT_TPS_LIST[@]}")
OPT_P99W_LIST=("${RESULT_P99W_LIST[@]}")
OPT_P99R_LIST=("${RESULT_P99R_LIST[@]}")

# ② 切换到基线，重编译，测试
apply_baseline
rebuild
run_bench "RocksDB 基线（WAL开启，无 BlockCache/BloomFilter）"
BASE_TPS="$RESULT_TPS"; BASE_P99W="$RESULT_P99W"; BASE_P99R="$RESULT_P99R"
BASE_TPS_LIST=("${RESULT_TPS_LIST[@]}")
BASE_P99W_LIST=("${RESULT_P99W_LIST[@]}")
BASE_P99R_LIST=("${RESULT_P99R_LIST[@]}")

# ③ 恢复优化配置
restore_optimized
rebuild > /dev/null 2>&1

# ─────────────────────────────────────────────────────
#  汇总对比
# ─────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  最终对比结果  (mode=readwrite, write_ratio=${WRITE_RATIO})${NC}"
echo -e "${BOLD}════════════════════════════════════════════════════════${NC}"
printf "  %-10s  %-10s  %-14s  %-14s  %-14s\n" \
  "配置" "Round" "TPS" "Write-p99" "Read-p99"
echo "  ──────────────────────────────────────────────────────"
for i in "${!OPT_TPS_LIST[@]}"; do
  printf "  %-10s  Round%-5s  %-14s  %-14s  %-14s\n" \
    "优化" "$((i+1))" "${OPT_TPS_LIST[$i]}" \
    "${OPT_P99W_LIST[$i]}ms" "${OPT_P99R_LIST[$i]}ms"
done
printf "  %-10s  %-10s  ${GREEN}%-14s${NC}  ${GREEN}%-14s${NC}  ${GREEN}%-14s${NC}\n" \
  "优化-均值" "" "$OPT_TPS" "${OPT_P99W}ms" "${OPT_P99R}ms"
echo "  ──────────────────────────────────────────────────────"
for i in "${!BASE_TPS_LIST[@]}"; do
  printf "  %-10s  Round%-5s  %-14s  %-14s  %-14s\n" \
    "基线" "$((i+1))" "${BASE_TPS_LIST[$i]}" \
    "${BASE_P99W_LIST[$i]}ms" "${BASE_P99R_LIST[$i]}ms"
done
printf "  %-10s  %-10s  %-14s  %-14s  %-14s\n" \
  "基线-均值" "" "$BASE_TPS" "${BASE_P99W}ms" "${BASE_P99R}ms"
echo "  ──────────────────────────────────────────────────────"

# 计算提升比例
tps_lift=$(awk "BEGIN{printf \"%.1f\", ($OPT_TPS-$BASE_TPS)*100/$BASE_TPS}")
p99r_lift=$(awk "BEGIN{printf \"%.1f\", ($BASE_P99R-$OPT_P99R)*100/$BASE_P99R}")
p99w_lift=$(awk "BEGIN{printf \"%.1f\", ($BASE_P99W-$OPT_P99W)*100/$BASE_P99W}")

echo ""
echo -e "  TPS      变化: ${tps_lift}%"
echo -e "  Write p99变化: ${p99w_lift}% 降低"
echo -e "  Read  p99变化: ${p99r_lift}% 降低  ${BOLD}← 核心优化收益${NC}"
echo ""
echo -e "${BOLD}════════════════════════════════════════════════════════${NC}"
