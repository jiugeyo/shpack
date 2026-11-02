#!/usr/bin/env bash
# 一键构建：宿主编译器构建 packer/builder，目标架构编译器构建静态 stub。
#
#   ./build.sh                       本机架构
#   ./build.sh --arch aarch64        交叉构建 Android/arm64 用的 stub
#   ./build.sh --arch armv7 --with-runtime
#   ./build.sh --arch i386
#   ./build.sh --builder             构建 4 架构单文件 builder（dist/shpack-builder）
#
# 交叉编译器查找顺序：build-tools/<arch>-g++ > $CROSS_COMPILE > 系统交叉编译器。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
ARCH=$(uname -m)
case "$ARCH" in
  amd64) ARCH=x86_64;;
  arm64) ARCH=aarch64;;
  armv7l|armv8l) ARCH=armv7;;
  i686) ARCH=i386;;
esac
WITH_RUNTIME=0
WITH_BUILDER=0
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2;;
    --with-runtime) WITH_RUNTIME=1; shift;;
    --builder) WITH_BUILDER=1; shift;;
    --jobs) JOBS=$2; shift 2;;
    -h|--help) sed -n '2,13p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done

cd "$ROOT"
uname_m=$(uname -m)
case "$uname_m" in
  amd64) uname_m=x86_64;;
  arm64) uname_m=aarch64;;
  armv8l|armv7l) uname_m=armv7;;
esac

if [ "$ARCH" != "$uname_m" ] && [ -z "${CROSS_COMPILE:-}" ] && [ ! -x "build-tools/$ARCH-g++" ]; then
  if command -v "aarch64-linux-gnu-g++" >/dev/null 2>&1 && [ "$ARCH" = aarch64 ]; then
    export CROSS_COMPILE=aarch64-linux-gnu-
  elif command -v "arm-linux-gnueabihf-g++" >/dev/null 2>&1 && [ "$ARCH" = armv7 ]; then
    export CROSS_COMPILE=arm-linux-gnueabihf-
  elif command -v "i686-linux-gnu-g++" >/dev/null 2>&1 && [ "$ARCH" = i386 ]; then
    export CROSS_COMPILE=i686-linux-gnu-
  else
    echo "错误: 找不到 $ARCH 的交叉编译器。" >&2
    echo "  无 root 方案: bash scripts/fetch-cross.sh --arch $ARCH" >&2
    echo "  或安装: sudo apt install g++-aarch64-linux-gnu / g++-arm-linux-gnueabihf / g++-i686-linux-gnu" >&2
    exit 1
  fi
fi

echo "== 构建 packer/builder/bundler（宿主）与 stub（$ARCH）=="
make -j"$JOBS" ARCH="$ARCH"
make check ARCH="$ARCH"

if [ "$WITH_RUNTIME" = 1 ]; then
  echo "== 获取 $ARCH 静态 bash/busybox =="
  bash scripts/fetch-static.sh --arch "$ARCH" --out "runtime/$ARCH"
fi

if [ "$WITH_BUILDER" = 1 ]; then
  echo "== 生成单文件 builder =="
  bash scripts/make-builder.sh
fi

cat <<EOF

完成：
  packer  : build/host/shpack
  builder : build/host/shpack-builder（未内嵌 bundle）
  stub    : build/$ARCH/shpack-stub

打包：
  ./pack.sh --arch $ARCH -s your-script.sh -o your-app
  ./shpack-builder -s your-script.sh -o your-app -a $ARCH
EOF
