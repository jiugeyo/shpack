#!/usr/bin/env bash
# 在无需 root 的情况下，从 Ubuntu 仓库下载并组装交叉工具链（用于构建目标 stub）。
#
#   scripts/fetch-cross.sh --arch armv7
#   scripts/fetch-cross.sh --arch i386
#
# 产物：
#   .cross/<arch>/root/            解包后的交叉编译器与 sysroot
#   build-tools/<arch>-g++         可直接用于 make TARGET_CXX=... 的包装器
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2;;
    -h|--help) sed -n '2,12p' "$0"; exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done
[ -n "$ARCH" ] || { echo "--arch 必填" >&2; exit 2; }

case "$ARCH" in
  armv7)
    TRIPLE=arm-linux-gnueabihf
    PKGS="binutils-arm-linux-gnueabihf cpp-13-arm-linux-gnueabihf
          gcc-13-arm-linux-gnueabihf g++-13-arm-linux-gnueabihf
          libgcc-13-dev-armhf-cross libc6-dev-armhf-cross
          libstdc++-13-dev-armhf-cross linux-libc-dev-armhf-cross"
    ;;
  i386)
    TRIPLE=i686-linux-gnu
    PKGS="binutils-i686-linux-gnu cpp-13-i686-linux-gnu
          gcc-13-i686-linux-gnu g++-13-i686-linux-gnu
          libgcc-13-dev-i386-cross libc6-dev-i386-cross
          libstdc++-13-dev-i386-cross linux-libc-dev-i386-cross"
    ;;
  *) echo "不支持: $ARCH（可选 armv7 / i386）" >&2; exit 2;;
esac

OUT="$ROOT/.cross/$ARCH"
DEBS="$OUT/debs"
PREFIX="$OUT/root"
mkdir -p "$DEBS" "$PREFIX" "$ROOT/build-tools"

echo "== 下载交叉工具链 ($ARCH, $TRIPLE) ==" >&2
cd "$DEBS"
for p in $PKGS; do
  if ls "${p}"_*.deb >/dev/null 2>&1; then continue; fi
  apt-get download "$p" >/dev/null 2>&1 || echo "警告: 下载失败 $p" >&2
done

echo "== 解包 ==" >&2
for d in ./*.deb; do
  [ -f "$d" ] || continue
  dpkg-deb -x "$d" "$PREFIX"
done

CC=$(find "$PREFIX/usr/bin" -maxdepth 1 -name "${TRIPLE}-g++-*" -type f | sort -V | tail -1)
if [ -z "$CC" ]; then
  echo "找不到 ${TRIPLE}-g++" >&2
  exit 1
fi
REL_CC=${CC#"$PREFIX"}
GCC_VER=${CC##*-}
GCC_DIR="$PREFIX/usr/lib/gcc-cross/$TRIPLE/$GCC_VER"
[ -d "$GCC_DIR" ] || GCC_DIR=$(dirname "$(find "$PREFIX/usr/lib/gcc-cross" -name cc1plus | head -1)")
# 交叉 binutils 的 as/ld 依赖 host 侧 libbfd/libopcodes，解包后需要显式 LD_LIBRARY_PATH
LIBDIR=$(dirname "$(find "$PREFIX" -name 'libbfd-*.so*' 2>/dev/null | head -1)")
[ -n "$LIBDIR" ] || LIBDIR="$PREFIX/usr/lib"

cat >"$ROOT/build-tools/$ARCH-g++" <<EOF
#!/bin/sh
PREFIX="$PREFIX"
export PATH="\$PREFIX/usr/bin:\$PATH"
export LD_LIBRARY_PATH="$LIBDIR:\${LD_LIBRARY_PATH:-}"
exec "\$PREFIX$REL_CC" --sysroot="\$PREFIX" -B"$GCC_DIR/" "\$@"
EOF
chmod +x "$ROOT/build-tools/$ARCH-g++"

echo "== 编译测试 ==" >&2
cat >"$OUT/test.cpp" <<'EOF'
#include <cstdio>
#include <string>
int main() { std::string s = "ok"; std::printf("%s\n", s.c_str()); return 0; }
EOF
if "$ROOT/build-tools/$ARCH-g++" -static -O2 -o "$OUT/test" "$OUT/test.cpp" 2>"$OUT/test.log"; then
  file "$OUT/test"
  echo "工具链就绪: build-tools/$ARCH-g++"
else
  echo "编译测试失败，日志: $OUT/test.log" >&2
  tail -20 "$OUT/test.log" >&2
  exit 1
fi
