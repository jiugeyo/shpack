#!/bin/bash
# 简单基准：对比「打包产物」与「直接 bash 跑脚本」的启动开销
set -u
cd "$(dirname "$0")/.." || exit 1
APP=${1:-build/hello}
BASH_BIN=${2:-runtime/x86_64/bash}
SCRIPT=${3:-examples/hello.sh}
N=${N:-30}

[ -x "$BASH_BIN" ] || BASH_BIN=prebuilt/x86_64/bash

bench() {
  local desc=$1
  shift
  local start end i
  start=$(date +%s%N)
  for i in $(seq 1 "$N"); do "$@" >/dev/null 2>&1; done
  end=$(date +%s%N)
  echo "$desc: $(( (end - start) / N / 1000000 )) ms/次（$N 次平均）"
}

[ -x "$APP" ] || { echo "缺少 $APP"; exit 1; }
[ -x "$BASH_BIN" ] || { echo "缺少 $BASH_BIN"; exit 1; }

bench "直接 bash  $SCRIPT" "$BASH_BIN" "$SCRIPT"
bench "shpack 产物 $APP" "$APP"
