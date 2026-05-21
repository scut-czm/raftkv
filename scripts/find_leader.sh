#!/bin/bash
# 找到最新的 "Became LEADER" 所在的日志文件，推断 Leader 端口和 PID
set -e

LEADER_PORT=""
for PORT in 8200 8201 8202; do
  LOG="/tmp/raftkv_$PORT.log"
  if [ ! -f "$LOG" ]; then continue; fi
  # 统计 Became LEADER 的次数，取最新的
  COUNT=$(grep -c "Became LEADER" "$LOG" 2>/dev/null) || COUNT=0
  if [ "$COUNT" -gt 0 ]; then
    # 检查是否也出现了 "Stopped being LEADER"（说明已让出）
    STOP_COUNT=$(grep -c "Stopped being LEADER" "$LOG" 2>/dev/null) || STOP_COUNT=0
    if [ "$COUNT" -gt "$STOP_COUNT" ]; then
      LEADER_PORT=$PORT
    fi
  fi
done

if [ -z "$LEADER_PORT" ]; then
  echo "ERROR: 未找到 Leader" >&2
  exit 1
fi

# 通过端口找 PID
LEADER_PID=$(pgrep -f "kv_server --port=$LEADER_PORT" | head -1)
if [ -z "$LEADER_PID" ]; then
  echo "ERROR: 未找到 Leader 进程 (port=$LEADER_PORT)" >&2
  exit 1
fi
if ! kill -0 "$LEADER_PID" 2>/dev/null; then
  echo "ERROR: Leader 进程 (port=$LEADER_PORT, pid=$LEADER_PID) 已死亡" >&2
  exit 1
fi
PROC_STATE=$(grep '^State:' "/proc/$LEADER_PID/status" 2>/dev/null | awk '{print $2}')
if [ "$PROC_STATE" = "Z" ]; then
  echo "ERROR: Leader 进程 (port=$LEADER_PORT, pid=$LEADER_PID) 是僵尸进程" >&2
  exit 1
fi

echo "LEADER_PORT=$LEADER_PORT"
echo "LEADER_PID=$LEADER_PID"

# 供其他脚本 source 使用
export LEADER_PORT LEADER_PID
