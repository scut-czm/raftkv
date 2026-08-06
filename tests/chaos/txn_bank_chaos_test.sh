#!/bin/bash
# 端到端事务混沌验证：银行转账 + 随机 kill/重启节点 + 快照总额不变量
# + 结束后离线检查 lock/write CF。
#
# 用法: bash tests/chaos/txn_bank_chaos_test.sh [压测秒数，默认 60]
# 前置: make kv_server bank_chaos_test mvcc_db_check

SCRIPT_DIR=$(dirname "$0")
ROOT_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
SCRIPTS_DIR="$ROOT_DIR/scripts"
CONF="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"
DURATION=${1:-60}
KILL_INTERVAL_S=10   # 每轮：跑 10s → kill 一个节点 → 5s 后拉起 → 循环
RECOVER_WAIT_S=5

PASS=0
FAIL=0
log_pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
log_fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

for BIN in kv_server bank_chaos_test mvcc_db_check; do
  if [ ! -x "$BUILD_DIR/$BIN" ]; then
    echo "ERROR: 缺少 $BUILD_DIR/$BIN，请先 make $BIN"
    exit 2
  fi
done

echo "=============================================="
echo "RaftKV 事务混沌测试（银行转账不变量）"
echo "时长: ${DURATION}s  故障注入间隔: ${KILL_INTERVAL_S}s"
echo "=============================================="

# ── 干净启动集群 ──────────────────────────────────────────────────
"$SCRIPTS_DIR/stop_cluster.sh" 2>/dev/null || true
sleep 1
rm -rf /tmp/raftkv_data_*
# kill -9 是本测试的核心手段，因此必须让 braft 在应答前 fsync 日志：
# raft_sync=false 时日志尾部可能还在用户态 append buffer / 页缓存之外，
# 被 kill -9 的节点会丢掉已被多数派确认的日志，后续多数派重组时这些
# 「已提交」的事务凭空消失（转账只生效一半 → 总额不守恒），
# 那是持久性配置问题，不是事务实现的 bug。
export RAFT_SYNC=true
export SNAPSHOT_INTERVAL_S=3600
"$SCRIPTS_DIR/start_cluster.sh"
sleep 5

restart_node() {
  local PORT=$1
  "$BUILD_DIR/kv_server" \
    --port="$PORT" \
    --ip=127.0.0.1 \
    --group=RaftKVGroup \
    --conf="127.0.0.1:8200:0,127.0.0.1:8201:0,127.0.0.1:8202:0" \
    --data_path="/tmp/raftkv_data_$PORT" \
    --raft_enable_leader_lease=true \
    --election_timeout_ms=1500 \
    --snapshot_interval_s=3600 \
    --raft_sync=true \
    --raft_sync_meta=true \
    --raft_max_append_buffer_size=4194304 \
    --raft_apply_batch=64 \
    >> "/tmp/raftkv_$PORT.log" 2>&1 &
}

# ── 后台混沌注入：随机 kill 一个节点，5s 后拉起（始终保 majority）──
CHAOS_LOG=/tmp/raftkv_bank_chaos.log
: > "$CHAOS_LOG"
chaos_loop() {
  local END=$(( $(date +%s) + DURATION - KILL_INTERVAL_S ))
  local PORTS=(8200 8201 8202)
  local KILLS=0
  while [ "$(date +%s)" -lt "$END" ]; do
    sleep "$KILL_INTERVAL_S"
    local P=${PORTS[$((RANDOM % 3))]}
    local PID
    PID=$(pgrep -f "kv_server --port=$P" | head -1 || true)
    if [ -n "$PID" ]; then
      echo "[chaos $(date '+%H:%M:%S')] kill -9 节点 $P (pid=$PID)" | tee -a "$CHAOS_LOG"
      kill -9 "$PID" 2>/dev/null || true
      KILLS=$((KILLS + 1))
    fi
    sleep "$RECOVER_WAIT_S"
    if ! pgrep -f "kv_server --port=$P" >/dev/null; then
      echo "[chaos $(date '+%H:%M:%S')] 重启节点 $P" | tee -a "$CHAOS_LOG"
      restart_node "$P"
    fi
  done
  echo "$KILLS" > /tmp/raftkv_bank_chaos_kills
}
chaos_loop &
CHAOS_PID=$!

# ── 前台跑银行转账压测（内含审计线程 + 最终审计）───────────────────
echo ""
echo "=== 测试 1：32 线程随机转账 + 快照总额不变量（混沌中） ==="
BANK_RC=0
"$BUILD_DIR/bank_chaos_test" \
  --peers="$CONF" \
  --accounts=16 \
  --threads=32 \
  --duration_s="$DURATION" \
  --initial=1000 || BANK_RC=$?

wait "$CHAOS_PID" 2>/dev/null || true
KILLS=$(cat /tmp/raftkv_bank_chaos_kills 2>/dev/null || echo 0)
echo "  故障注入次数: $KILLS"

if [ "$BANK_RC" -eq 0 ]; then
  log_pass "混沌期间及最终快照总额恒等于初始总额"
else
  log_fail "总额不变量被破坏（bank_chaos_test 退出码 $BANK_RC）"
fi

# ── 等日志追平后停集群，做离线 CF 检查 ──────────────────────────────
echo ""
echo "=== 测试 2：离线检查 lock CF 为空 / write CF 无悬空 start_ts ==="
# 等所有副本把日志 apply 完再停集群：刚被拉起的节点要从头重放日志
# （server_main 启动时会清空 RocksDB），没追平就做离线检查会把「还没重放到」
# 误判成残锁/丢数据。用 braft 内建的 /raft_stat 读 known_applied_index。
wait_replicas_caught_up() {
  local DEADLINE=$(( $(date +%s) + 120 ))
  while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    local VALS=()
    local ALL_UP=true
    for PORT in 8200 8201 8202; do
      local STAT
      STAT=$(curl -s -m 2 "http://127.0.0.1:$PORT/raft_stat" || true)
      local APPLIED LAST
      APPLIED=$(echo "$STAT" | grep -o "known_applied_index: [0-9]*" | head -1 | grep -o "[0-9]*")
      LAST=$(echo "$STAT" | grep -o "last_log_index: [0-9]*" | head -1 | grep -o "[0-9]*")
      if [ -z "$APPLIED" ] || [ -z "$LAST" ]; then
        ALL_UP=false
        break
      fi
      if [ "$APPLIED" -lt "$LAST" ]; then
        ALL_UP=false
        break
      fi
      VALS+=("$APPLIED")
    done
    if [ "$ALL_UP" = true ] && [ "${VALS[0]}" = "${VALS[1]}" ] &&
       [ "${VALS[1]}" = "${VALS[2]}" ]; then
      echo "  三副本已追平: known_applied_index=${VALS[0]}"
      return 0
    fi
    sleep 1
  done
  echo "  WARN: 等待副本追平超时，离线检查结果可能包含未追平节点"
  return 1
}
wait_replicas_caught_up
# 注意：不能用 stop_cluster.sh（它会 rm -rf 数据目录，离线检查就没东西可查了）。
# 用 SIGINT 优雅退出：RocksDB 关闭 WAL 后，只有正常关库才会把 memtable 刷盘。
pkill -INT -f "kv_server --port" || true
for _ in $(seq 1 20); do
  pgrep -f "kv_server --port" >/dev/null || break
  sleep 1
done
pkill -9 -f "kv_server --port" 2>/dev/null || true
sleep 1

CF_OK=true
for PORT in 8200 8201 8202; do
  DB="/tmp/raftkv_data_$PORT/rocksdb"
  if [ ! -d "$DB" ]; then
    echo "  节点 $PORT 无数据目录"
    CF_OK=false
    continue
  fi
  echo "  --- 节点 $PORT ---"
  if ( set -o pipefail; "$BUILD_DIR/mvcc_db_check" --db_path="$DB" | sed 's/^/    /' ); then
    :
  else
    CF_OK=false
  fi
done

if [ "$CF_OK" = true ]; then
  log_pass "全部节点 lock CF 为空、write CF 无悬空 start_ts"
else
  log_fail "存在残锁或悬空 write 记录（见上方输出）"
fi

echo ""
echo "=============================================="
echo "测试结果: PASS=$PASS  FAIL=$FAIL"
echo "=============================================="
[ "$FAIL" -eq 0 ] || exit 1
