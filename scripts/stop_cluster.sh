#!/bin/bash
pkill -f "kv_server --port" || true
echo "All kv_server nodes stopped."

sleep 1
rm -rf /tmp/raftkv_data_*
echo "Data directories cleaned."