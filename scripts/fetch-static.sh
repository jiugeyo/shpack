#!/usr/bin/env bash
# 获取「官方仓库里的」静态 bash 与 busybox（无需 root）。
#
#   scripts/fetch-static.sh                    当前架构（apt-get download）
#   scripts/fetch-static.sh --arch aarch64     交叉架构（从 ports.ubuntu.com 下载）
#   scripts/fetch-static.sh --out runtime/arm64
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
    -h|--help) sed -n '2,10p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done
OUT=${OUT:-$ROOT/runtime/$ARCH}
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

case "$ARCH" in
  x86_64|amd64) DEBARCH=amd64;;
  aarch64|arm64) DEBARCH=arm64;;
  armv7|armhf) DEBARCH=armhf;;
  *) echo "不支持的架构: $ARCH" >&2; exit 2;;
esac

fetch_host() {
  ( cd "$TMP" && apt-get download bash-static busybox-static ) || {
    echo "apt-get download 失败：请确认已 apt-get update，且 universe 源可用" >&2
    return 1
  }
}

fetch_ports() {
  local pkg=$1 comp url
  for comp in main universe; do
    # awk 找到 Filename 后会提前退出，导致 curl 报 23 (SIGPIPE)；
    # 这里显式忽略管道状态，赋值本身仍然有效。
    url=$(curl -fsSL --max-time 60 \
            "http://ports.ubuntu.com/ubuntu-ports/dists/noble/$comp/binary-$DEBARCH/Packages.gz" \
          | gzip -dc \
          | awk -v p="$pkg" '$0 == "Package: " p { found=1 } found && /^Filename:/ { print $2; exit }') || true
    if [ -n "${url:-}" ]; then
      echo "$url"
      return 0
    fi
  done
  return 1
}

if [ "$DEBARCH" = "$(dpkg --print-architecture)" ]; then
  echo "从本机 apt 源下载 bash-static / busybox-static ($DEBARCH) ..." >&2
  fetch_host
else
  echo "从 ports.ubuntu.com 下载 $DEBARCH 静态包 ..." >&2
  for pkg in bash-static busybox-static; do
    url=$(fetch_ports "$pkg") || { echo "找不到 $pkg ($DEBARCH)" >&2; exit 1; }
    echo "  $pkg: $url" >&2
    curl -fL --retry 3 -o "$TMP/$pkg.deb" "http://ports.ubuntu.com/ubuntu-ports/$url"
  done
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
  find "$TMP/extract" -type f -name '*bash*' -o -name 'busybox' 2>/dev/null >&2
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
