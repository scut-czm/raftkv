#!/bin/bash
# 找到最新的 Leader 选出日志，推断 Leader 端口和 PID
set -e

LEADER_PORT=""
for PORT in 8200 8201 8202; do
  LOG="/tmp/raftkv_$PORT.log"
  if [ ! -f "$LOG" ]; then continue; fi

  # 【优化点 1】使用 -Ei 开启扩展正则与大小写免疫，完美兼容自定义日志与 braft 原生 "leader starts to" 日志
  #  同时改用 | wc -l 彻底规避 grep -c 在 0 匹配时抛出非 0 状态码触发 set -e 的隐式猝死 Bug
  COUNT=$(grep -Ei "Became LEADER|leader starts to" "$LOG" 2>/dev/null | wc -l)
  STOP_COUNT=$(grep -Ei "Stopped being LEADER|leader steps down" "$LOG" 2>/dev/null | wc -l)

  if [ "$COUNT" -gt "$STOP_COUNT" ]; then
    LEADER_PORT=$PORT
    break # 捕获到当前唯一活跃的主节点，果断破壳退出
  fi
done

if [ -z "$LEADER_PORT" ]; then
  echo "ERROR: 未找到 Leader (日志未匹配成功)" >&2
  exit 1
fi

# 【优化点 2】使用更弹性的正则匹配，包容 --port=8200 或 --port 8200 的各种命令行启动版型
LEADER_PID=$(pgrep -f "kv_server.*port=$LEADER_PORT" | head -1)

if [ -z "$LEADER_PID" ]; then
  echo "ERROR: 未找到 Leader 进程 (port=$LEADER_PORT)" >&2
  exit 1
fi

if ! kill -0 "$LEADER_PID" 2>/dev/null; then
  echo "ERROR: Leader 进程 (port=$LEADER_PORT, pid=$LEADER_PID) 已死亡" >&2
  exit 1
fi

# 【核心修正 3】将所有的描述性文本通过 >&2 物理排挤到标准错误流（Stderr）中
# 这样既能在终端屏幕上亮出精美的排查上下文，又绝对不会污染外层脚本的变量捕获器
echo "========================================" >&2
echo "Raft Cluster Metrics Discovery:" >&2
echo "  Active Leader Port : $LEADER_PORT" >&2
echo "  Active Leader PID  : $LEADER_PID" >&2
echo "========================================" >&2

# 【终极输出】Stdout 管道有且仅输出干净的纯数字 PID，完美交付给 LEADER_PID=$(...)
echo "$LEADER_PID"