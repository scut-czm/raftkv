#!/bin/bash
set -e

SCRIPT_DIR=$(dirname "$0")
ROOT_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
SCRIPTS_DIR="$ROOT_DIR/scripts"
CLIENT="$BUILD_DIR/kv_client"
CONF="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

PASS=0
FAIL=0

log_pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
log_fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# ── 确保集群干净启动 ──────────────────────────────────────────────
restart_cluster() {
  "$SCRIPTS_DIR/stop_cluster.sh" 2>/dev/null || true
  sleep 1
  rm -rf /tmp/raftkv_data_*
  "$SCRIPTS_DIR/start_cluster.sh"
  sleep 5
}

# ── 等待写入恢复（轮询探测），返回耗时毫秒 ──────────────────────
wait_for_write_recovery() {
  local max_wait_ms=${1:-10000}
  local start_ms elapsed
  start_ms=$(date +%s%3N)
  elapsed=0

  while [ $elapsed -lt $max_wait_ms ]; do
    if timeout 1s "$CLIENT" --peers="$CONF" --command=put \
        --key=probe_$(date +%s%N) --value=ok --timeout_ms=800 2>/dev/null | grep -q "PUT OK"; then
      echo "$elapsed"
      return 0
    fi
    sleep 0.1
    elapsed=$(( $(date +%s%3N) - start_ms ))
  done

  echo "$elapsed"
  return 1
}

# ════════════════════════════════════════════════════════════════════
echo "=============================================="
echo "RaftKV 混沌测试"
echo "=============================================="


# ── 测试 1：Leader 故障切换 ──────────────────────────────────────
echo ""
echo "=== 测试 1：Kill Leader → 等待新 Leader → 验证写入恢复 ==="

restart_cluster

# 先写入一些数据
for i in $(seq 1 10); do
  "$CLIENT" --peers="$CONF" --command=put --key="pre_$i" --value="v$i" \
    >/dev/null 2>&1
done

# 找到 Leader
eval "$(bash "$SCRIPTS_DIR/find_leader.sh")"
echo "  当前 Leader: port=$LEADER_PORT pid=$LEADER_PID"

# Kill Leader
START_MS=$(date +%s%3N)
kill -9 "$LEADER_PID"
echo "  Leader killed at $(date '+%H:%M:%S')"

# 等待写入恢复
ELAPSED_MS=$(wait_for_write_recovery 10000) || true
END_MS=$(date +%s%3N)
ACTUAL_MS=$((END_MS - START_MS))

echo "  故障切换耗时: ${ACTUAL_MS}ms"
if [ "$ACTUAL_MS" -lt 3000 ]; then
  log_pass "Leader 故障切换 < 3s (实际 ${ACTUAL_MS}ms)"
else
  log_fail "Leader 故障切换 >= 3s (实际 ${ACTUAL_MS}ms)"
fi

# 验证之前的数据还在
RESULT=$("$CLIENT" --peers="$CONF" --command=get --key=pre_5 2>/dev/null)
if echo "$RESULT" | grep -q "value=v5"; then
  log_pass "故障切换后数据完整"
else
  log_fail "故障切换后数据丢失"
fi

# ── 测试 2：Kill Follower → 写入不受影响 ────────────────────────
echo ""
echo "=== 测试 2：Kill 1 个 Follower → 写入不受影响 ==="

restart_cluster

# 找到 Leader，Kill 一个非 Leader 节点
eval "$(bash "$SCRIPTS_DIR/find_leader.sh")"
echo "  Leader: port=$LEADER_PORT"

FOLLOWER_PORT=""
for PORT in 8200 8201 8202; do
  if [ "$PORT" != "$LEADER_PORT" ]; then
    FOLLOWER_PORT=$PORT
    break
  fi
done

FOLLOWER_PID=$(pgrep -f "kv_server --port=$FOLLOWER_PORT" | head -1)
echo "  Kill Follower: port=$FOLLOWER_PORT pid=$FOLLOWER_PID"
kill -9 "$FOLLOWER_PID"
sleep 1

# 持续写入应不受影响（majority 仍存在）
WRITE_OK=true
for i in $(seq 1 20); do
  if ! "$CLIENT" --peers="$CONF" --command=put \
      --key="f_test_$i" --value="val_$i" 2>/dev/null | grep -q "PUT OK"; then
    WRITE_OK=false
    break
  fi
done

if [ "$WRITE_OK" = true ]; then
  log_pass "Kill 1 Follower 后写入正常（majority 存活）"
else
  log_fail "Kill 1 Follower 后写入异常"
fi


# ── 测试 3：Kill 2 Follower → majority 丢失 → 写入阻塞 ─────────
echo ""
echo "=== 测试 3：Kill 第 2 个 Follower → 写入应阻塞 → 恢复后写入恢复 ==="

# Kill 第二个 Follower
FOLLOWER2_PORT=""
for PORT in 8200 8201 8202; do
  if [ "$PORT" != "$LEADER_PORT" ] && [ "$PORT" != "$FOLLOWER_PORT" ]; then
    FOLLOWER2_PORT=$PORT
    break
  fi
done

FOLLOWER2_PID=$(pgrep -f "kv_server --port=$FOLLOWER2_PORT" | head -1)
echo "  Kill Follower2: port=$FOLLOWER2_PORT pid=$FOLLOWER2_PID"
kill -9 "$FOLLOWER2_PID"
sleep 1

# 写入应该超时/失败（只剩 Leader 1 个节点，无 majority）
if "$CLIENT" --peers="$CONF" --command=put \
    --key="blocked" --value="test" --timeout_ms=2000 2>/dev/null | grep -q "PUT OK"; then
  log_fail "majority 丢失后写入应阻塞但仍成功"
else
  log_pass "majority 丢失后写入正确阻塞"
fi

# 恢复一个 Follower
echo "  恢复 Follower: port=$FOLLOWER_PORT"
DATA_DIR="/tmp/raftkv_data_$FOLLOWER_PORT"
"$BUILD_DIR/kv_server" \
  --port="$FOLLOWER_PORT" \
  --ip=127.0.0.1 \
  --group=RaftKVGroup \
  --conf="127.0.0.1:8200:0,127.0.0.1:8201:0,127.0.0.1:8202:0" \
  --data_path="$DATA_DIR" \
  --election_timeout_ms=3000 \
  --snapshot_interval_s=3600 \
  > "/tmp/raftkv_$FOLLOWER_PORT.log" 2>&1 &
sleep 3

# 写入应恢复
ELAPSED_MS=$(wait_for_write_recovery 10000) || true
if [ "$ELAPSED_MS" -lt 10000 ]; then
  log_pass "恢复 Follower 后写入恢复 (${ELAPSED_MS}ms)"
else
  log_fail "恢复 Follower 后写入仍失败"
fi

# ── 测试 4：多次 Leader 切换 ─────────────────────────────────────
echo ""
echo "=== 测试 4：连续 3 次 Kill Leader → 每次切换 < 3s ==="

restart_cluster

ALL_SWITCH_OK=true
PREV_KILLED_PORT=""
for round in 1 2 3; do
  # 恢复上一轮被杀的节点，保证集群始终有 majority
  if [ -n "$PREV_KILLED_PORT" ]; then
    DATA_DIR="/tmp/raftkv_data_$PREV_KILLED_PORT"
    "$BUILD_DIR/kv_server" \
      --port="$PREV_KILLED_PORT" \
      --ip=127.0.0.1 \
      --group=RaftKVGroup \
      --conf="127.0.0.1:8200:0,127.0.0.1:8201:0,127.0.0.1:8202:0" \
      --data_path="$DATA_DIR" \
      --raft_enable_leader_lease=true \
      --election_timeout_ms=500 \
      --snapshot_interval_s=3600 \
      --raft_sync=false \
      --raft_max_append_buffer_size=4194304 \
      > "/tmp/raftkv_$PREV_KILLED_PORT.log" 2>&1 &
    echo "  恢复节点: port=$PREV_KILLED_PORT"
  fi
  sleep 2

  FIND_OUTPUT=$(bash "$SCRIPTS_DIR/find_leader.sh" 2>/dev/null) || {
    log_fail "第 $round 轮未找到 Leader"
    ALL_SWITCH_OK=false
    break
  }
  eval "$FIND_OUTPUT"

  echo "  第 $round 轮 Leader: port=$LEADER_PORT pid=$LEADER_PID"
  START_MS=$(date +%s%3N)
  kill -9 "$LEADER_PID"
  PREV_KILLED_PORT=$LEADER_PORT

  ELAPSED_MS=$(wait_for_write_recovery 10000) || true
  END_MS=$(date +%s%3N)
  ACTUAL_MS=$((END_MS - START_MS))
  echo "  切换耗时: ${ACTUAL_MS}ms"

  if [ "$ACTUAL_MS" -ge 3000 ]; then
    ALL_SWITCH_OK=false
  fi
done

if [ "$ALL_SWITCH_OK" = true ]; then
  log_pass "3 次连续 Leader 切换均 < 3s"
else
  log_fail "存在 Leader 切换 >= 3s"
fi

# ── 结果汇总 ──────────────────────────────────────────────────────
echo ""
echo "=============================================="
echo "测试结果: PASS=$PASS  FAIL=$FAIL"
echo "=============================================="

"$SCRIPTS_DIR/stop_cluster.sh"

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi