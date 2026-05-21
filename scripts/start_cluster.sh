#!/bin/bash
set -e

BINARY=$(dirname "$0")/../build/kv_server
CONF="127.0.0.1:8200:0,127.0.0.1:8201:0,127.0.0.1:8202:0"

if [ ! -f "$BINARY" ]; then
  echo "ERROR: 未找到 $BINARY，请先执行 make kv_server"
  exit 1
fi

for PORT in 8200 8201 8202; do
  DATA_DIR="/tmp/raftkv_data_$PORT"
  mkdir -p "$DATA_DIR"
  "$BINARY" \
    --port="$PORT" \
    --ip=127.0.0.1 \
    --group=RaftKVGroup \
    --conf="$CONF" \
    --data_path="$DATA_DIR" \
    --raft_enable_leader_lease=true \
    --election_timeout_ms=1500 \
    --snapshot_interval_s=120 \
    --raft_sync=false \
    --raft_sync_meta=true \
    --raft_max_append_buffer_size=4194304 \
    --raft_apply_batch=64 \
    > "/tmp/raftkv_$PORT.log" 2>&1 &
  echo "Node $PORT started (pid=$!, log=/tmp/raftkv_$PORT.log)"
  sleep 1
done

echo ""
echo "Cluster started. CONF: $CONF"
echo "观察 Leader 选出："
echo "  grep 'Became LEADER' /tmp/raftkv_820*.log"
