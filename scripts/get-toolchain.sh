#!/usr/bin/env bash
# 下载 musl.cc 提供的静态交叉工具链（无需 root）。
# 用法: get-toolchain.sh [aarch64|x86_64|armv7]
set -euo pipefail

ARCH=${1:-aarch64}
DEST=${SHPACK_TOOLCHAIN_DIR:-$PWD/.toolchain}
mkdir -p "$DEST"

case "$ARCH" in
  aarch64) PKG=aarch64-linux-musl-cross;;
  x86_64)  PKG=x86_64-linux-musl-cross;;
  armv7)   PKG=armv7l-linux-musleabihf-cross;;
  *) echo "不支持的架构: $ARCH" >&2; exit 2;;
esac

if [ -x "$DEST/$PKG/bin/${PKG%%-cross}-gcc" ]; then
  echo "工具链已存在: $DEST/$PKG" >&2
  echo "$DEST/$PKG"
  exit 0
fi

URL="https://musl.cc/$PKG.tgz"
echo "下载 $URL ..." >&2
curl -fL --retry 3 --retry-delay 2 -o "$DEST/$PKG.tgz" "$URL"
tar -xzf "$DEST/$PKG.tgz" -C "$DEST"
rm -f "$DEST/$PKG.tgz"
echo "$DEST/$PKG"
