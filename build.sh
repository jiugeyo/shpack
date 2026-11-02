#!/usr/bin/env bash
# 一键构建：宿主编译器构建 packer，目标架构编译器构建静态 stub。
#
#   ./build.sh                       本机架构
#   ./build.sh --arch aarch64        交叉构建 Android/arm64 用的 stub
#   ./build.sh --arch aarch64 --with-runtime
#                                    同时下载 arm64 的静态 bash/busybox
#
# 交叉编译器查找顺序：$CROSS_COMPILE 前缀 > aarch64-linux-gnu-g++ > musl 工具链。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
ARCH=$(uname -m)
WITH_RUNTIME=0
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2;;
    --with-runtime) WITH_RUNTIME=1; shift;;
    --jobs) JOBS=$2; shift 2;;
    -h|--help) sed -n '2,12p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done

cd "$ROOT"
uname_m=$(uname -m)
if [ "$ARCH" = "$uname_m" ]; then
  :
elif [ -z "${CROSS_COMPILE:-}" ]; then
  if command -v "aarch64-linux-gnu-g++" >/dev/null 2>&1 && [ "$ARCH" = aarch64 ]; then
    export CROSS_COMPILE=aarch64-linux-gnu-
  elif command -v "arm-linux-gnueabihf-g++" >/dev/null 2>&1 && [ "$ARCH" = armv7 ]; then
    export CROSS_COMPILE=arm-linux-gnueabihf-
  else
    echo "错误: 找不到 $ARCH 的交叉编译器。" >&2
    echo "  可安装: sudo apt install g++-aarch64-linux-gnu" >&2
    echo "  或使用 musl.cc 工具链: scripts/get-toolchain.sh $ARCH" >&2
    exit 1
  fi
fi

echo "== 构建 packer（宿主）与 stub（$ARCH）=="
make -j"$JOBS" ARCH="$ARCH"
make check ARCH="$ARCH"

if [ "$WITH_RUNTIME" = 1 ]; then
  echo "== 获取 $ARCH 静态 bash/busybox =="
  bash scripts/fetch-static.sh --arch "$ARCH" --out "runtime/$ARCH"
fi

cat <<EOF

完成：
  packer : build/host/shpack
  stub   : build/$ARCH/shpack-stub

打包：
  ./pack.sh --arch $ARCH -s your-script.sh -o your-app
  ./your-app --help
EOF
