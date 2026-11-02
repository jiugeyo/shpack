#!/bin/bash
# 信号测试：收到 TERM 时写标记再退出
trap 'echo term > "$SIG_MARK"; exit 0' TERM
echo "ready pid=$$ dir=$SHPACK_DIR"
for _ in $(seq 1 100); do
  sleep 0.1
done
echo "timeout" >&2
exit 9
