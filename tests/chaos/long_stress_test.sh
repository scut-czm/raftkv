#!/bin/bash
set -e

SCRIPT_DIR=$(dirname "$0")
ROOT_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
SCRIPTS_DIR="$ROOT_DIR/scripts"
PERF_TEST="$BUILD_DIR/perf_test"
CONF="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

DURATION_UNIT=${2:-h}
if [ "$DURATION_UNIT" = "m" ]; then
  DURATION_SECS=$(( ${1:-72} * 60 ))
  DURATION_LABEL="${1:-72}min"
  KILL_INTERVAL_S=180
  RECOVERY_WAIT_S=120
  SNAPSHOT_INTERVAL_S=120
else
  DURATION_SECS=$(( ${1:-72} * 3600 ))
  DURATION_LABEL="${1:-72}h"
  KILL_INTERVAL_S=1800
  RECOVERY_WAIT_S=300
  SNAPSHOT_INTERVAL_S=3600
fi

LOG_FILE="/tmp/raftkv_stress_$(date +%Y%m%d_%H%M%S).log"
START_TS=$(date +%s)

log() {
  local elapsed=$(( $(date +%s) - START_TS ))
  echo "[t=${elapsed}s $(date '+%H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

# ── 启动集群 ──────────────────────────────────────────────────────
log "启动集群..."
"$SCRIPTS_DIR/stop_cluster.sh" 2>/dev/null || true
sleep 1
rm -rf /tmp/raftkv_data_*
"$SCRIPTS_DIR/start_cluster.sh"
sleep 5

# ── 后台监控服务端日志，实时捕获关键事件 ─────────────────────────────
# 注意：不能用 $() 包装，因为 tail -qF 永不退出会导致 $() 永久阻塞
# 直接启动后台管道，>> 写文件，$! 取 PID
tail -qF /tmp/raftkv_8200.log /tmp/raftkv_8201.log /tmp/raftkv_8202.log 2>/dev/null \
| grep --line-buffered -iE \
    "step.?down|higher.?term|dead.?node|quorum|LEADER|CANDIDATE|FOLLOWER|lease|snapshot|election|term [0-9]+" \
| while IFS= read -r line; do
    echo "[SRV t=$(( $(date +%s) - START_TS ))s] $line" >> "$LOG_FILE"
  done &
MONITOR_PID=$!
log "服务端日志监控 PID=$MONITOR_PID"

# ── 后台压测（32线程，256B value） ─────────────────────────────────
log "启动后台压测: 32 线程, 256B value"
"$PERF_TEST" \
  --peers="$CONF" \
  --mode=write \
  --threads=32 \
  --duration_s="$DURATION_SECS" \
  --value_size=256 \
  --timeout_ms=2000 \
  --max_retry=20 \
  > "/tmp/raftkv_stress_perf.log" 2>&1 &
PERF_PID=$!
log "perf_test PID=$PERF_PID"

# ── 故障注入循环 ──────────────────────────────────────────────────
TOTAL_KILLS=0
END_TIME=$((START_TS + DURATION_SECS))

while [ "$(date +%s)" -lt "$END_TIME" ]; do
  log "等待 ${KILL_INTERVAL_S}s 后注入故障..."
  sleep "$KILL_INTERVAL_S"

  if [ "$(date +%s)" -ge "$END_TIME" ]; then break; fi

  # 随机选择一个节点 Kill
  PORTS=(8200 8201 8202)
  RANDOM_IDX=$((RANDOM % 3))
  KILL_PORT=${PORTS[$RANDOM_IDX]}
  KILL_PID=$(pgrep -f "kv_server --port=$KILL_PORT" | head -1 || true)

  if [ -n "$KILL_PID" ]; then
    log "Kill 节点: port=$KILL_PORT pid=$KILL_PID"
    # 记录 kill 前各节点最新状态
    for P in 8200 8201 8202; do
      tail -3 "/tmp/raftkv_$P.log" 2>/dev/null | while IFS= read -r line; do
        echo "[t=$(( $(date +%s) - START_TS ))s PRE-KILL srv$P] $line" >> "$LOG_FILE"
      done
    done
    kill -9 "$KILL_PID" 2>/dev/null || true
    TOTAL_KILLS=$((TOTAL_KILLS + 1))
  else
    log "节点 $KILL_PORT 已停止，跳过"
  fi

  log "等待 ${RECOVERY_WAIT_S}s 后恢复节点..."
  sleep "$RECOVERY_WAIT_S"

  # 恢复节点（保留数据目录）
  DATA_DIR="/tmp/raftkv_data_$KILL_PORT"
  "$BUILD_DIR/kv_server" \
    --port="$KILL_PORT" \
    --ip=127.0.0.1 \
    --group=RaftKVGroup \
    --conf="127.0.0.1:8200:0,127.0.0.1:8201:0,127.0.0.1:8202:0" \
    --data_path="$DATA_DIR" \
    --raft_enable_leader_lease=true \
    --election_timeout_ms=1500 \
    --snapshot_interval_s="$SNAPSHOT_INTERVAL_S" \
    --raft_sync=false \
    --raft_sync_meta=true \
    --raft_max_append_buffer_size=4194304 \
    --raft_apply_batch=64 \
    >> "/tmp/raftkv_$KILL_PORT.log" 2>&1 &
  RECOVER_PID=$!
  log "节点 $KILL_PORT 已恢复 (pid=$RECOVER_PID)，等待 3s 验证存活..."
  sleep 3
  if kill -0 "$RECOVER_PID" 2>/dev/null; then
    PROC_STATE=$(grep '^State:' "/proc/$RECOVER_PID/status" 2>/dev/null | awk '{print $2}')
    if [ "$PROC_STATE" = "Z" ]; then
      log "WARN: 节点 $KILL_PORT 为僵尸进程，启动失败！"
      tail -10 "/tmp/raftkv_$KILL_PORT.log" | while IFS= read -r line; do log "      $line"; done
    else
      log "节点 $KILL_PORT 存活确认 (pid=$RECOVER_PID, state=$PROC_STATE)"
    fi
  else
    log "WARN: 节点 $KILL_PORT 已退出，启动失败！"
    tail -10 "/tmp/raftkv_$KILL_PORT.log" | while IFS= read -r line; do log "      $line"; done
  fi
done

# ── 等待压测结束 ──────────────────────────────────────────────────
log "等待 perf_test 结束..."
wait "$PERF_PID" 2>/dev/null || true

kill "$MONITOR_PID" 2>/dev/null || true

# ── 收集结果 ──────────────────────────────────────────────────────
log ""
log "========== 压测结果 =========="
log "持续时间: ${DURATION_LABEL}"
log "故障注入次数: $TOTAL_KILLS"
log "perf_test 输出:"
tail -20 /tmp/raftkv_stress_perf.log | tee -a "$LOG_FILE"
log "==============================="

# ── 服务端关键日志汇总 ─────────────────────────────────────────────
log ""
log "========== 服务端关键事件 =========="
for P in 8200 8201 8202; do
  log "--- 节点 $P ---"
  grep -iE "step.?down|higher.?term|dead.?node|Majority|LEADER|CANDIDATE|snapshot|ERAFT" \
    "/tmp/raftkv_$P.log" 2>/dev/null | tail -40 | while IFS= read -r line; do
      log "  $line"
    done
done
log "====================================="

"$SCRIPTS_DIR/stop_cluster.sh"
log "集群已停止。详细日志: $LOG_FILE"