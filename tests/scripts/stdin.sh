#!/bin/bash
# 读取 stdin 并回显（测试包装器是否透传标准输入）
echo "stdin:[$(cat)]"
echo "arg1:[${1:-}]"
