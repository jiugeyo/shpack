#!/usr/bin/env bash
# 生成「单文件交互式 builder」：把 host packer + 多架构 stub/bash/busybox
# 追加成一个 ELF。
#
#   scripts/make-builder.sh
#   scripts/make-builder.sh --arches "x86_64 aarch64 armv7 i386"
#   scripts/make-builder.sh --aarch64-host        # 额外产出 aarch64 主机版
#
# 组件查找顺序：build/<arch> → prebuilt/stub/<arch>；runtime/<arch> → prebuilt/<arch>。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

ARCHES="x86_64 aarch64 armv7 i386"
OUT=dist/shpack-builder
AARCH64_HOST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --arches) ARCHES=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --aarch64-host) AARCH64_HOST=1; shift;;
    -h|--help) sed -n '2,10p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done

mkdir -p dist build
echo "== 构建 host 工具 ==" >&2
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" builder bundler packer >/dev/null

args=()
summary=()
for arch in $ARCHES; do
  stub="build/$arch/shpack-stub"
  if [ ! -f "$stub" ]; then
    if [ -f "prebuilt/stub/$arch/shpack-stub" ]; then
      stub="prebuilt/stub/$arch/shpack-stub"
    elif make ARCH="$arch" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" stub >/dev/null 2>&1; then
      :
    else
      echo "跳过 $arch：没有 stub（先 make ARCH=$arch 或 scripts/fetch-cross.sh --arch $arch）" >&2
      continue
    fi
  fi

  bash_bin="runtime/$arch/bash"
  bb_bin="runtime/$arch/busybox"
  [ -f "$bash_bin" ] || bash_bin="prebuilt/$arch/bash"
  [ -f "$bb_bin" ] || bb_bin="prebuilt/$arch/busybox"
  if [ ! -f "$bash_bin" ] || [ ! -f "$bb_bin" ]; then
    echo "跳过 $arch：缺少静态 bash/busybox" >&2
    continue
  fi

  args+=(--arch "$arch" --stub "$stub" --bash "$bash_bin" --busybox "$bb_bin")
  summary+=("$arch: $(basename "$stub") + $(basename "$bash_bin")")
done

if [ ${#args[@]} -eq 0 ]; then
  echo "没有可用架构，无法生成 builder" >&2
  exit 1
fi

echo "== 生成 bundle ==" >&2
build/host/shpack-bundler -o build/shpack.bundle "${args[@]}"

echo "== 追加到 builder ==" >&2
cp build/host/shpack-builder "$OUT"
strip -s "$OUT" 2>/dev/null || true
cat build/shpack.bundle >>"$OUT"
chmod 755 "$OUT"

echo
echo "已生成 $OUT ($(du -h "$OUT" | cut -f1))"
"$OUT" info | head -20

if [ "$AARCH64_HOST" = 1 ] && command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
  echo
  echo "== 构建 aarch64 主机版 builder ==" >&2
  ZSTD_A="$ROOT/.deps/zstd-aarch64"
  if [ ! -f "$ZSTD_A/lib/libzstd.a" ]; then
    bash "$ROOT/scripts/fetch-zstd.sh" --arch aarch64 --out "$ZSTD_A" >/dev/null
  fi
  mkdir -p build/host-aarch64
  make HOST_DIR=build/host-aarch64 HOST_CXX=aarch64-linux-gnu-g++ \
       ZSTD_CPPFLAGS="-I$ZSTD_A/include" \
       ZSTD_LIBS="-L$ZSTD_A/lib -lzstd -pthread" builder >/dev/null
  cp build/host-aarch64/shpack-builder dist/shpack-builder-aarch64
  AARCH64_STRIP=$(command -v aarch64-linux-gnu-strip || echo strip)
  "$AARCH64_STRIP" -s dist/shpack-builder-aarch64 2>/dev/null || true
  cat build/shpack.bundle >>dist/shpack-builder-aarch64
  chmod 755 dist/shpack-builder-aarch64
  file dist/shpack-builder-aarch64
fi
