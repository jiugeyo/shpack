#!/usr/bin/env bash
# 从源码构建「纯静态」bash 与 busybox。
# 默认下载 musl.cc 交叉工具链（musl libc，体积最小，Android 推荐）；
# 也可以用 --cc 指定任意交叉编译器（如 aarch64-linux-gnu- 得到 glibc 静态版）。
# 用法:
#   scripts/build-runtime.sh --arch aarch64 [--cc PREFIX] [--out DIR] [--jobs N]
#   scripts/build-runtime.sh --arch aarch64 --only bash
# 产物: $OUT/bash, $OUT/busybox（均为静态 ELF）
set -euo pipefail

ARCH=aarch64
OUT=""
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
ONLY=""
CC_PREFIX=""
BASH_VER=5.2
BUSYBOX_VER=1.36.1

while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --jobs) JOBS=$2; shift 2;;
    --only) ONLY=$2; shift 2;;
    --cc) CC_PREFIX=$2; shift 2;;
    -h|--help) sed -n '2,7p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/runtime/$ARCH}
CACHE=${SHPACK_CACHE:-$ROOT/.cache}
BUILD=${SHPACK_BUILD:-$ROOT/.build/$ARCH}
mkdir -p "$OUT" "$CACHE/src" "$BUILD"

case "$ARCH" in
  aarch64) TRIPLE=aarch64-linux-musl;;
  x86_64)  TRIPLE=x86_64-linux-musl;;
  armv7)   TRIPLE=armv7l-linux-musleabihf;;
  *) echo "不支持的架构: $ARCH" >&2; exit 2;;
esac

if [ -n "$CC_PREFIX" ]; then
  if ! _cc=$(command -v "${CC_PREFIX}gcc"); then
    echo "找不到交叉编译器: ${CC_PREFIX}gcc" >&2
    exit 1
  fi
  CROSS="$(cd "$(dirname "$_cc")" && pwd)/${CC_PREFIX}"
else
  TOOLCHAIN=$("$ROOT/scripts/get-toolchain.sh" "$ARCH")
  CROSS="$TOOLCHAIN/bin/${TRIPLE}-"
  export PATH="$TOOLCHAIN/bin:$PATH"
fi
if [ ! -x "${CROSS}gcc" ]; then
  echo "找不到交叉编译器: ${CROSS}gcc" >&2
  exit 1
fi
HOST_TRIPLE=$("${CROSS}gcc" -dumpmachine)
echo "编译器: ${CROSS}gcc ($HOST_TRIPLE)" >&2

fetch() { # url 文件名
  local url=$1 name=$2
  if [ ! -f "$CACHE/src/$name" ]; then
    echo "下载 $url" >&2
    curl -fL --retry 3 --retry-delay 2 -o "$CACHE/src/$name.part" "$url"
    mv "$CACHE/src/$name.part" "$CACHE/src/$name"
  fi
}

build_bash() {
  local src=$CACHE/src/bash-$BASH_VER.tar.gz dir=$BUILD/bash-$BASH_VER
  fetch "https://ftp.gnu.org/gnu/bash/bash-$BASH_VER.tar.gz" "bash-$BASH_VER.tar.gz"
  rm -rf "$dir"
  mkdir -p "$dir"
  tar -xzf "$src" -C "$dir" --strip-components=1
  echo "== 配置 bash ($ARCH) ==" >&2
  (
    cd "$dir"
    CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
    LDFLAGS="-static -s" \
    ./configure --build="$(uname -m)-pc-linux-gnu" --host="$HOST_TRIPLE" \
      --without-bash-malloc --enable-static-link --disable-nls \
      --prefix=/ --localstatedir=/var 2>&1 | tail -5
    make -j"$JOBS" 2>&1 | tail -5
  )
  "${CROSS}strip" -s "$dir/bash" 2>/dev/null || true
  cp "$dir/bash" "$OUT/bash"
  chmod 755 "$OUT/bash"
  echo "bash -> $OUT/bash" >&2
}

build_busybox() {
  local src=$CACHE/src/busybox-$BUSYBOX_VER.tar.bz2 dir=$BUILD/busybox-$BUSYBOX_VER
  fetch "https://busybox.net/downloads/busybox-$BUSYBOX_VER.tar.bz2" "busybox-$BUSYBOX_VER.tar.bz2"
  rm -rf "$dir"
  mkdir -p "$dir"
  tar -xjf "$src" -C "$dir" --strip-components=1
  echo "== 配置 busybox ($ARCH) ==" >&2
  (
    cd "$dir"
    make CROSS_COMPILE="$CROSS" defconfig >/dev/null
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    grep -q '^CONFIG_STATIC=y' .config || echo 'CONFIG_STATIC=y' >> .config
    # 新版内核头文件（>= 6.5）删除了 CBQ 相关常量，busybox 1.36 的 tc 无法编译；
    # 这个 applet 很少用到，直接关掉以保证构建成功。
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
    yes '' | make CROSS_COMPILE="$CROSS" oldconfig >/dev/null 2>&1 || true
    make -j"$JOBS" CROSS_COMPILE="$CROSS" 2>&1 | tail -5
  )
  "${CROSS}strip" -s "$dir/busybox" 2>/dev/null || true
  cp "$dir/busybox" "$OUT/busybox"
  chmod 755 "$OUT/busybox"
  echo "busybox -> $OUT/busybox" >&2
}

case "$ONLY" in
  bash) build_bash;;
  busybox) build_busybox;;
  "") build_bash; build_busybox;;
  *) echo "--only 只支持 bash|busybox" >&2; exit 2;;
esac

echo "== 结果 ==" >&2
file "$OUT/bash" "$OUT/busybox" 2>/dev/null || true
