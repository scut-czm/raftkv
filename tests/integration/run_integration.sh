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
TOTAL=0

log_test() {
  TOTAL=$((TOTAL + 1))
  echo ""
  echo "=== 测试 $TOTAL: $1 ==="
}

log_pass() {
  PASS=$((PASS + 1))
  echo "  PASS: $1"
}


log_fail() {
  FAIL=$((FAIL + 1))
  echo "  FAIL: $1"
}

# ── 工具函数 ──────────────────────────────────────────────────────────

start_fresh_cluster() {
  "$SCRIPTS_DIR/stop_cluster.sh" 2>/dev/null || true
  sleep 1
  rm -rf /tmp/raftkv_data_*
  "$SCRIPTS_DIR/start_cluster.sh"
  sleep 6  # 等待选举完成
}

stop_cluster() {
  "$SCRIPTS_DIR/stop_cluster.sh" 2>/dev/null || true
}

# 在指定端口上弱一致读
get_from_node() {
  local port=$1
  local key=$2
  "$CLIENT" --peers="127.0.0.1:$port" --command=get --key="$key" \
    --linearizable=false 2>/dev/null
}

# 通过集群自动 redirect 写入
put_via_cluster() {
  local key=$1
  local value=$2
  "$CLIENT" --peers="$CONF" --command=put --key="$key" --value="$value" \
    2>/dev/null
}
# ══════════════════════════════════════════════════════════════════════
echo "=============================================="
echo "RaftKV 集成测试"
echo "=============================================="

# ── 测试 1：[P0] 基本一致性 ──────────────────────────────────────────
log_test "[P0] 3 节点写入 100 条，三节点数据一致"

start_fresh_cluster

# 写入 100 条
WRITE_FAIL=0
for i in $(seq 1 100); do
  RESULT=$(put_via_cluster "ikey_$i" "ival_$i")
  if ! echo "$RESULT" | grep -q "PUT OK"; then
    WRITE_FAIL=$((WRITE_FAIL + 1))
  fi
done

if [ "$WRITE_FAIL" -gt 0 ]; then
  log_fail "写入失败 $WRITE_FAIL 条"
else
  log_pass "100 条写入全部成功"
fi

# 从三个节点分别弱一致读，验证一致
CONSISTENCY_OK=true
for PORT in 8200 8201 8202; do
  RESULT=$(get_from_node "$PORT" "ikey_50")
  if echo "$RESULT" | grep -q "value=ival_50"; then
    echo "  Node $PORT: ikey_50 = ival_50 OK"
  else
    echo "  Node $PORT: ikey_50 读取异常: $RESULT"
    CONSISTENCY_OK=false
  fi
done

if [ "$CONSISTENCY_OK" = true ]; then
  log_pass "三节点数据一致"
else
  log_fail "三节点数据不一致"
fi

stop_cluster

# ── 测试 2：[P0] Kill Follower → 写入不受影响 → 恢复 → 数据同步 ────
log_test "[P0] Kill Follower → 写入不受影响 → 恢复后数据同步"

start_fresh_cluster

# 找 Leader
LEADER_PORT=""
for PORT in 8200 8201 8202; do
  if grep -q "Became LEADER" "/tmp/raftkv_$PORT.log" 2>/dev/null; then
    STOP_COUNT=$(grep -c "Stopped being LEADER" "/tmp/raftkv_$PORT.log" 2>/dev/null || true)
    START_COUNT=$(grep -c "Became LEADER" "/tmp/raftkv_$PORT.log" 2>/dev/null || true)
    if [ "$START_COUNT" -gt "$STOP_COUNT" ]; then
      LEADER_PORT=$PORT
    fi
  fi
done

if [ -z "$LEADER_PORT" ]; then
  log_fail "未找到 Leader"
  stop_cluster
else
  echo "  Leader: $LEADER_PORT"

  # 选择一个 Follower Kill
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

  # Kill Follower 后持续写入应正常（majority 仍存活）
  WRITE_OK=true
  for i in $(seq 1 50); do
    RESULT=$(put_via_cluster "fk_$i" "fv_$i")
    if ! echo "$RESULT" | grep -q "PUT OK"; then
      WRITE_OK=false
      break
    fi
  done

  if [ "$WRITE_OK" = true ]; then
    log_pass "Kill Follower 后 50 条写入正常"
  else
    log_fail "Kill Follower 后写入异常"
  fi

# 恢复 Follower
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
  echo "  Follower $FOLLOWER_PORT 已恢复 (pid=$!)"
  sleep 5  # 等待日志同步

  # 从恢复的 Follower 读取数据
  RESULT=$(get_from_node "$FOLLOWER_PORT" "fk_25")
  if echo "$RESULT" | grep -q "value=fv_25"; then
    log_pass "恢复的 Follower 数据已同步"
  else
    log_fail "恢复的 Follower 数据未同步: $RESULT"
  fi

  stop_cluster
fi

# ── 测试 3：[P1] Kill Leader → 新 Leader → 写入恢复 ─────────────────
log_test "[P1] Kill Leader → 重新选举 → 写入恢复"

start_fresh_cluster

# 先写入一些数据
for i in $(seq 1 20); do
  put_via_cluster "lk_$i" "lv_$i" >/dev/null
done

# 找 Leader
LEADER_PORT=""
for PORT in 8200 8201 8202; do
  START_COUNT=$(grep -c "Became LEADER" "/tmp/raftkv_$PORT.log" 2>/dev/null || true)
  STOP_COUNT=$(grep -c "Stopped being LEADER" "/tmp/raftkv_$PORT.log" 2>/dev/null || true)
  if [ "$START_COUNT" -gt "$STOP_COUNT" ]; then
    LEADER_PORT=$PORT
  fi
done

if [ -z "$LEADER_PORT" ]; then
  log_fail "未找到 Leader"
else
  LEADER_PID=$(pgrep -f "kv_server --port=$LEADER_PORT" | head -1)
  echo "  Kill Leader: port=$LEADER_PORT pid=$LEADER_PID"
  kill -9 "$LEADER_PID"

  # 等待新 Leader 选举
  echo "  等待重新选举..."
  sleep 5

  # 写入应恢复
  RECOVERED=false
  for attempt in $(seq 1 10); do
    RESULT=$(put_via_cluster "after_kill" "ok" 2>/dev/null)
    if echo "$RESULT" | grep -q "PUT OK"; then
      RECOVERED=true
      break
    fi
    sleep 1
  done

  if [ "$RECOVERED" = true ]; then
    log_pass "Kill Leader 后写入已恢复"
  else
    log_fail "Kill Leader 后写入未恢复"
  fi

  # 验证之前的数据仍在
  RESULT=$("$CLIENT" --peers="$CONF" --command=get --key="lk_10" \
    --linearizable=false 2>/dev/null)
  if echo "$RESULT" | grep -q "value=lv_10"; then
    log_pass "Kill Leader 后历史数据完整"
  else
    log_fail "Kill Leader 后历史数据丢失"
  fi
fi

stop_cluster

# ── 测试 4：[P2] Scan redirect ───────────────────────────────────────
log_test "[P2] Scan 通过 redirect 正确返回数据"

start_fresh_cluster

# 写入一批有序数据
for i in $(seq 1 30); do
  KEY=$(printf "scan_%03d" "$i")
  put_via_cluster "$KEY" "v$i" >/dev/null
done
sleep 2

# Scan
RESULT=$("$CLIENT" --peers="$CONF" --command=scan \
  --start_key="scan_" --end_key="scan_z" --limit=100 2>/dev/null)

GOT_COUNT=$(echo "$RESULT" | grep -c "scan_" || echo 0)
if [ "$GOT_COUNT" -ge 30 ]; then
  log_pass "Scan 返回 $GOT_COUNT 条（>= 30）"
else
  log_fail "Scan 只返回 $GOT_COUNT 条（期望 >= 30）"
fi

stop_cluster

# ── 测试 5：[P1] Delete 后 Get 验证 ──────────────────────────────────
log_test "[P1] Put → Delete → Get 验证删除生效"

start_fresh_cluster

put_via_cluster "del_test" "to_delete" >/dev/null
sleep 1

# Delete
RESULT=$("$CLIENT" --peers="$CONF" --command=delete --key="del_test" 2>/dev/null)
if echo "$RESULT" | grep -q "DELETE OK"; then
  echo "  Delete 成功"
else
  echo "  Delete 异常: $RESULT"
fi
sleep 1

# Get 验证已删除
RESULT=$("$CLIENT" --peers="$CONF" --command=get --key="del_test" 2>/dev/null)
if echo "$RESULT" | grep -q "not found"; then
  log_pass "Delete 后 Get 返回 not found"
else
  log_fail "Delete 后仍能读到数据: $RESULT"
fi

stop_cluster


# ── 测试 6：[P0] 线性一致读 vs 弱一致读 ──────────────────────────────
log_test "[P0] 写入后立即线性一致读，保证读到最新值"

start_fresh_cluster

STALE_COUNT=0
for i in $(seq 1 20); do
  put_via_cluster "lin_$i" "linval_$i" >/dev/null

  # 立即线性一致读
  RESULT=$("$CLIENT" --peers="$CONF" --command=get --key="lin_$i" \
    --linearizable=true 2>/dev/null)
  if ! echo "$RESULT" | grep -q "value=linval_$i"; then
    STALE_COUNT=$((STALE_COUNT + 1))
    echo "  WARN: lin_$i 线性一致读未读到最新值"
  fi
done

if [ "$STALE_COUNT" -eq 0 ]; then
  log_pass "20 次写后线性一致读均读到最新值"
else
  log_fail "线性一致读 stale 次数: $STALE_COUNT / 20"
fi

stop_cluster

# ══════════════════════════════════════════════════════════════════════
echo ""
echo "=============================================="
echo "集成测试结果: PASS=$PASS  FAIL=$FAIL  TOTAL=$TOTAL"
echo "=============================================="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi

