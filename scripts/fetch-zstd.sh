#!/usr/bin/env bash
# 获取交叉架构的静态 libzstd（仅用于 aarch64 主机版 builder 链接 tar.zst）
#
#   scripts/fetch-zstd.sh --arch aarch64 --out .deps/zstd-aarch64
#
# 产物：<out>/include/zstd.h, <out>/lib/libzstd.a
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=aarch64
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    -h|--help) sed -n '2,8p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done
OUT=${OUT:-$ROOT/.deps/zstd-$ARCH}
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/tmp"

case "$ARCH" in
  aarch64|arm64) DEBARCH=arm64; TRIPLE=aarch64-linux-gnu; BASE=http://ports.ubuntu.com/ubuntu-ports;;
  armv7|armhf)   DEBARCH=armhf; TRIPLE=arm-linux-gnueabihf; BASE=http://ports.ubuntu.com/ubuntu-ports;;
  i386)          DEBARCH=i386; TRIPLE=i386-linux-gnu; BASE=http://archive.ubuntu.com/ubuntu;;
  x86_64|amd64)  DEBARCH=amd64; TRIPLE=x86_64-linux-gnu; BASE=http://archive.ubuntu.com/ubuntu;;
  *) echo "不支持的架构: $ARCH" >&2; exit 2;;
esac

TMP="$OUT/tmp"
url=""
for comp in main universe; do
  curl -fsSL --max-time 120 "$BASE/dists/noble/$comp/binary-$DEBARCH/Packages.gz" -o "$TMP/$comp.gz" || continue
  url=$(gzip -dc "$TMP/$comp.gz" \
        | awk -v p=libzstd-dev '$0 == "Package: " p { f=1 } f && /^Filename:/ { print $2; exit }') || true
  [ -n "$url" ] && break
done

if [ -z "${url:-}" ]; then
  echo "找不到 libzstd-dev ($DEBARCH)" >&2
  exit 1
fi
echo "下载 $url" >&2
curl -fL --retry 3 --max-time 180 -o "$TMP/libzstd-dev.deb" "$BASE/$url"
dpkg-deb -x "$TMP/libzstd-dev.deb" "$TMP/root"
find "$TMP/root" -name 'zstd.h' -exec cp {} "$OUT/include/" \;
find "$TMP/root" -name 'libzstd.a' -exec cp {} "$OUT/lib/" \;
rm -rf "$TMP"
if [ ! -f "$OUT/include/zstd.h" ] || [ ! -f "$OUT/lib/libzstd.a" ]; then
  echo "提取失败：缺少 zstd.h 或 libzstd.a" >&2
  exit 1
fi
echo "libzstd 就绪: $OUT"
