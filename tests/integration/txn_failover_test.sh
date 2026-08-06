#!/bin/bash
# tests/integration/txn_failover_test.sh
# 验证：1) 向 follower 发事务 RPC 返回 redirect；2) kill leader 后事务状态不丢。
set -e
DIR=$(dirname "$0")
BUILD="$DIR/../../build"

# 说明：scripts/find_leader.sh 的 stdout 输出的是 leader 进程 PID（不是地址），
# 且日志推断在"节点被 kill 后重启"时会被残留的 Became LEADER 记录误导。
# 这里改用 RPC 探测：带 snapshot_ts 的 Get 会走 RediretIfNotLeader，
# leader 直接 success=1，follower 回 redirect=真 leader 地址。
find_leader_addr() {
  for PORT in 8200 8201 8202; do
    ADDR="127.0.0.1:$PORT"
    PID=$(pgrep -f "kv_server.*port=$PORT" | head -1)
    [ -n "$PID" ] || continue
    OUT=$("$BUILD/txn_probe" --addr="$ADDR" --op=get --key=__leader_probe__ \
          --snapshot_ts=1 || true)
    if echo "$OUT" | grep -q "success=1"; then
      echo "$ADDR"
      return 0
    fi
    R=$(echo "$OUT" | sed -n 's/.*redirect=\([^ ][^ ]*\).*/\1/p')
    if [ -n "$R" ]; then
      # redirect 是 braft PeerId 格式 ip:port:index（如 127.0.0.1:8202:0），
      # 只取前两段作为 RPC 地址，否则 brpc Channel Init 报 Invalid address。
      echo "$R" | cut -d: -f1,2
      return 0
    fi
  done
  echo "ERROR: 未找到存活的 Leader" >&2
  return 1
}

# 重启单个节点（参数与 scripts/start_cluster.sh 一致），保证集群维持多数派。
restart_node() {
  local PORT=$1
  "$BUILD/kv_server" \
    --port="$PORT" --ip=127.0.0.1 --group=RaftKVGroup \
    --conf="127.0.0.1:8200:0,127.0.0.1:8201:0,127.0.0.1:8202:0" \
    --data_path="/tmp/raftkv_data_$PORT" \
    --raft_enable_leader_lease=true --election_timeout_ms=1500 \
    --snapshot_interval_s=120 --raft_sync=false --raft_sync_meta=true \
    >> "/tmp/raftkv_$PORT.log" 2>&1 &
  echo "restarted node $PORT (pid=$!)"
}

bash "$DIR/../../scripts/stop_cluster.sh" || true
rm -rf /tmp/raftkv_data_820* /tmp/raftkv_820*.log
bash "$DIR/../../scripts/start_cluster.sh"
sleep 3

LEADER=$(find_leader_addr)
echo "leader = $LEADER"

# ---- 测试 1：向 follower 发 TxnPrewrite，应返回 redirect=leader ----
for PORT in 8200 8201 8202; do
  ADDR="127.0.0.1:$PORT"
  [ "$ADDR" == "$LEADER" ] && continue
  # 注意：txn_probe 对 success=0（含 redirect）返回非 0 退出码，
  # set -e 下命令替换失败会终止脚本，预期失败的探测必须加 || true。
  OUT=$("$BUILD/txn_probe" --addr="$ADDR" --op=prewrite --key=fk --value=fv \
        --primary=fk --start_ts=$((100<<18)) || true)
  echo "$OUT" | grep -q "redirect=$LEADER" \
    && echo "PASS: follower $ADDR redirect 正确" \
    || { echo "FAIL: follower $ADDR 未返回 redirect ($OUT)"; exit 1; }
done

# ---- 测试 2：leader 上完成 Prewrite+Commit，kill leader，新 leader 上可见 ----
"$BUILD/txn_probe" --addr="$LEADER" --op=prewrite --key=surv --value=alive \
    --primary=surv --start_ts=$((200<<18)) | grep -q success=1 || exit 1
"$BUILD/txn_probe" --addr="$LEADER" --op=commit --key=surv \
    --start_ts=$((200<<18)) --commit_ts=$((300<<18)) | grep -q success=1 || exit 1

LEADER_PORT=${LEADER##*:}
pkill -f "kv_server.*port=$LEADER_PORT" && echo "killed leader $LEADER"
sleep 5   # 等重新选主（election_timeout_ms=1500）

NEW_LEADER=$(find_leader_addr)
[ "$NEW_LEADER" != "$LEADER" ] || { echo "FAIL: 未选出新 leader"; exit 1; }
echo "new leader = $NEW_LEADER"

OUT=$("$BUILD/txn_probe" --addr="$NEW_LEADER" --op=get --key=surv \
      --snapshot_ts=$((400<<18)) || true)
echo "$OUT" | grep -q "value=alive" \
  && echo "PASS: leader 切换后事务提交不丢" \
  || { echo "FAIL: 新 leader 上读不到已提交数据 ($OUT)"; exit 1; }

# ---- 测试 3（加强）：只 Prewrite 未 Commit 时 kill leader，锁应仍在 ----
# 3 节点已 kill 掉 1 个，若再 kill 新 leader 只剩 1 节点，不足多数派无法选主。
# 先把第一个被 kill 的节点重启回来，保证 kill 后仍有 2/3 存活。
restart_node "$LEADER_PORT"
sleep 3

L=$NEW_LEADER
"$BUILD/txn_probe" --addr="$L" --op=prewrite --key=pend --value=x \
    --primary=pend --start_ts=$((500<<18)) | grep -q success=1 || exit 1
pkill -f "kv_server.*port=${L##*:}" && echo "killed leader $L"
sleep 5
L2=$(find_leader_addr)
OUT=$("$BUILD/txn_probe" --addr="$L2" --op=get --key=pend --snapshot_ts=$((600<<18)) || true)
echo "$OUT" | grep -q "locked=1" \
  && echo "PASS: 未提交锁在 failover 后仍然存在（可被 CheckTxnStatus 清理）" \
  || { echo "FAIL: 未提交锁丢失 ($OUT)"; exit 1; }

# ---- 测试 4：TSO failover——切主后发出的 ts 必须大于切主前的所有 ts ----
# 从 txn_probe 输出行提取 ts= 字段
parse_ts() { echo "$1" | sed -n 's/.*ts=\([0-9][0-9]*\).*/\1/p'; }

# 测试 3 又 kill 了一个节点，先把它重启回来，保证 kill 后仍有 2/3 存活。
restart_node "${L##*:}"
sleep 3
L3=$(find_leader_addr)
echo "tso test leader = $L3"

# 4a. follower 上取 TSO 应返回 redirect
for PORT in 8200 8201 8202; do
  ADDR="127.0.0.1:$PORT"
  [ "$ADDR" == "$L3" ] && continue
  OUT=$("$BUILD/txn_probe" --addr="$ADDR" --op=tso || true)
  echo "$OUT" | grep -q "redirect=$L3" \
    && echo "PASS: follower $ADDR tso redirect 正确" \
    || { echo "FAIL: follower $ADDR tso 未返回 redirect ($OUT)"; exit 1; }
  break   # 验证一个 follower 即可
done

# 4b. 同一 leader 上连续两次批量取号：第二次起点 ≥ 第一次起点 + count
OUT=$("$BUILD/txn_probe" --addr="$L3" --op=tso --count=100 || true)
echo "$OUT" | grep -q "success=1" || { echo "FAIL: tso 取号失败 ($OUT)"; exit 1; }
TS1=$(parse_ts "$OUT")
OUT=$("$BUILD/txn_probe" --addr="$L3" --op=tso --count=100 || true)
TS2=$(parse_ts "$OUT")
[ -n "$TS1" ] && [ -n "$TS2" ] && [ "$TS2" -ge $((TS1 + 100)) ] \
  && echo "PASS: 同一 leader 批量取号区间不重叠 (ts1=$TS1 ts2=$TS2)" \
  || { echo "FAIL: 批量取号区间重叠 (ts1=$TS1 ts2=$TS2)"; exit 1; }
MAX_BEFORE=$((TS2 + 100 - 1))   # 切主前已发出的最大 ts（区间 [TS2, TS2+100)）

# 4c. kill leader，新 leader 发出的 ts 必须 > 切主前所有 ts（不重复、不回退）
pkill -f "kv_server.*port=${L3##*:}" && echo "killed leader $L3"
sleep 5
L4=$(find_leader_addr)
[ "$L4" != "$L3" ] || { echo "FAIL: 未选出新 leader"; exit 1; }
echo "new leader = $L4"

OUT=$("$BUILD/txn_probe" --addr="$L4" --op=tso || true)
echo "$OUT" | grep -q "success=1" || { echo "FAIL: 新 leader tso 取号失败 ($OUT)"; exit 1; }
TS3=$(parse_ts "$OUT")
[ -n "$TS3" ] && [ "$TS3" -gt "$MAX_BEFORE" ] \
  && echo "PASS: failover 后 TSO 不重复不回退 (max_before=$MAX_BEFORE new_ts=$TS3)" \
  || { echo "FAIL: failover 后 TSO 回退/重复 (max_before=$MAX_BEFORE new_ts=$TS3)"; exit 1; }

bash "$DIR/../../scripts/stop_cluster.sh" || true
echo "ALL PASS"
