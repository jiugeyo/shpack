#!/usr/bin/env bash
# 获取「官方仓库里的」静态 bash 与 busybox（无需 root）。
#
#   scripts/fetch-static.sh --arch x86_64  --out runtime/x86_64
#   scripts/fetch-static.sh --arch aarch64 --out runtime/aarch64
#   scripts/fetch-static.sh --arch armv7   --out runtime/armv7
#   scripts/fetch-static.sh --arch i386    --out runtime/i386
#
# 说明：Ubuntu 的 bash-static / busybox-static 是 glibc 静态链接，可直接在
#       Android 内核上运行，体积略大于 musl 版本。追求最小体积/最干净依赖时，
#       请用 scripts/build-runtime.sh 从源码构建 musl 版本。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=$(uname -m)
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    -h|--help) sed -n '2,11p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done
OUT=${OUT:-$ROOT/runtime/$ARCH}
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ARCHIVE=http://archive.ubuntu.com/ubuntu
PORTS=http://ports.ubuntu.com/ubuntu-ports
case "$ARCH" in
  x86_64|amd64)   DEBARCH=amd64;  BASE=$ARCHIVE;;
  i386|i686|x86)  DEBARCH=i386;   BASE=$ARCHIVE;;
  aarch64|arm64)  DEBARCH=arm64;  BASE=$PORTS;;
  armv7|armhf)    DEBARCH=armhf;  BASE=$PORTS;;
  riscv64)        DEBARCH=riscv64; BASE=$PORTS;;
  *) echo "不支持的架构: $ARCH" >&2; exit 2;;
esac

fetch_host() {
  ( cd "$TMP" && apt-get download bash-static busybox-static ) || {
    echo "apt-get download 失败，尝试直接解析仓库索引..." >&2
    return 1
  }
}

IDXDIR="$TMP/idx"
mkdir -p "$IDXDIR"

index_file() {
  local comp=$1 f="$IDXDIR/$1.gz"
  if [ ! -s "$f" ]; then
    curl -fsSL --max-time 120 "$BASE/dists/noble/$comp/binary-$DEBARCH/Packages.gz" -o "$f" || return 1
  fi
  echo "$f"
}

fetch_url() {
  local pkg=$1 comp f url
  for comp in main universe; do
    f=$(index_file "$comp") || continue
    # awk 命中后提前退出会让 gzip 报 SIGPIPE；这里显式忽略管道状态。
    url=$(gzip -dc "$f" \
          | awk -v p="$pkg" '$0 == "Package: " p { found=1 } found && /^Filename:/ { print $2; exit }') || true
    if [ -n "${url:-}" ]; then
      echo "$url"
      return 0
    fi
  done
  return 1
}

fetch_remote() {
  local pkg url
  for pkg in bash-static busybox-static; do
    url=$(fetch_url "$pkg") || { echo "找不到 $pkg ($DEBARCH)" >&2; return 1; }
    echo "  $pkg: $url" >&2
    curl -fL --retry 3 --max-time 180 -o "$TMP/$pkg.deb" "$BASE/$url" || return 1
  done
}

echo "获取 $ARCH ($DEBARCH) 静态 bash/busybox ..." >&2
if [ "$DEBARCH" = "$(dpkg --print-architecture 2>/dev/null)" ] && [ "$BASE" = "$ARCHIVE" ]; then
  if ! fetch_host; then
    fetch_remote
  fi
else
  fetch_remote
fi

for deb in "$TMP"/*.deb; do dpkg-deb -x "$deb" "$TMP/extract"; done
for cand in usr/bin/bash-static bin/bash-static usr/bin/bash; do
  if [ -f "$TMP/extract/$cand" ]; then cp "$TMP/extract/$cand" "$OUT/bash"; break; fi
done
for cand in usr/bin/busybox bin/busybox; do
  if [ -f "$TMP/extract/$cand" ]; then cp "$TMP/extract/$cand" "$OUT/busybox"; break; fi
done
chmod 755 "$OUT/bash" "$OUT/busybox" 2>/dev/null || true

if [ ! -x "$OUT/bash" ] || [ ! -x "$OUT/busybox" ]; then
  echo "提取失败：$OUT 下缺少 bash/busybox" >&2
  exit 1
fi

echo "== 结果 ==" >&2
file "$OUT/bash" "$OUT/busybox"
for b in bash busybox; do
  if file -b "$OUT/$b" | grep -q 'statically linked'; then
    echo "$b 静态链接 OK" >&2
  else
    echo "警告: $b 可能不是静态链接" >&2
  fi
done
