#!/bin/bash
# Demo 录制脚本（配合 asciinema 或 terminalizer 使用）
# 使用方法：asciinema rec demo.cast -c "bash scripts/demo.sh"

set -e

echo "========================================="
echo "  RaftSQL Demo - 分布式 SQL 数据库"
echo "========================================="
echo ""
sleep 1

echo "# 1. 启动 3 节点集群"
echo '$ ./scripts/start_cluster.sh'
./scripts/start_cluster.sh
sleep 5
echo "集群已启动 ✅"
echo ""
sleep 1

PEERS="127.0.0.1:8200,127.0.0.1:8201,127.0.0.1:8202"

echo "# 2. CREATE TABLE"
echo '$ ./sql_client --sql="CREATE TABLE employees ..."'
./build/sql_client --peers=$PEERS \
  --sql="CREATE TABLE employees (id INT PRIMARY KEY, name VARCHAR(64), dept VARCHAR(32), salary INT)"
sleep 1

echo ""
echo "# 3. INSERT 数据"
./build/sql_client --peers=$PEERS \
  --sql="INSERT INTO employees (id, name, dept, salary) VALUES (1, 'Alice', 'Engineering', 120000)"
./build/sql_client --peers=$PEERS \
  --sql="INSERT INTO employees (id, name, dept, salary) VALUES (2, 'Bob', 'Marketing', 95000)"
./build/sql_client --peers=$PEERS \
  --sql="INSERT INTO employees (id, name, dept, salary) VALUES (3, 'Charlie', 'Engineering', 130000)"
./build/sql_client --peers=$PEERS \
  --sql="INSERT INTO employees (id, name, dept, salary) VALUES (4, 'Diana', 'Sales', 88000)"
./build/sql_client --peers=$PEERS \
  --sql="INSERT INTO employees (id, name, dept, salary) VALUES (5, 'Eve', 'Engineering', 115000)"
echo "5 rows inserted ✅"
sleep 1

echo ""
echo "# 4. SELECT 查询"
echo '$ SELECT name, salary FROM employees WHERE salary > 100000'
./build/sql_client --peers=$PEERS \
  --sql="SELECT name, salary FROM employees WHERE salary > 100000"
sleep 2

echo ""
echo "# 5. 聚合查询"
echo '$ SELECT COUNT(*) FROM employees'
./build/sql_client --peers=$PEERS \
  --sql="SELECT COUNT(*) FROM employees"
echo '$ SELECT SUM(salary) FROM employees'
./build/sql_client --peers=$PEERS \
  --sql="SELECT SUM(salary) FROM employees"
sleep 2

echo ""
echo "# 6. Kill Leader 测试故障切换"
LEADER_PID=$(./scripts/find_leader.sh 2>/dev/null || echo "")
if [ -n "$LEADER_PID" ]; then
  echo "Leader PID: $LEADER_PID"
  kill -9 $LEADER_PID
  echo "Leader killed! Waiting for re-election..."
  sleep 5
  echo "# 继续 SQL 查询（数据无丢失）"
  ./build/sql_client --peers=$PEERS \
    --sql="SELECT * FROM employees WHERE id = 1"
  echo "故障切换成功，数据完整 ✅"
fi

sleep 2
echo ""
echo "# 7. 清理"
./scripts/stop_cluster.sh
echo ""
echo "========================================="
echo "  Demo 完成！"
echo "========================================="