#!/usr/bin/env bash
# 在 Android / Linux 上展示运行环境（示例：证明无需系统 bash/coreutils）
set -u

get_prop() {
  if command -v getprop >/dev/null 2>&1; then
    getprop "$1" 2>/dev/null
  fi
}

echo "=== shpack runtime ==="
echo "bash        : $BASH_VERSION"
echo "self        : $SHPACK_SELF"
echo "mode        : $SHPACK_MODE"
echo "tmp dir     : ${SHPACK_DIR:-<memfd>}"
echo "script      : $SHPACK_SCRIPT"
echo "name        : $SHPACK_NAME"
echo "kernel      : $(uname -srm)"
echo "abi         : $(get_prop ro.product.cpu.abi || uname -m)"
echo "sdk         : $(get_prop ro.build.version.sdk || echo n/a)"
echo "device      : $(get_prop ro.product.model || echo n/a)"
echo "user        : $(id -un 2>/dev/null || id -u)"
echo "applets     : $(busybox --list 2>/dev/null | wc -l)"
echo "busybox     : $(busybox 2>&1 | head -1)"

echo
echo "=== 内置工具抽样 ==="
printf 'sed  : '; printf 'a-b\n' | sed 's/-/+/'
printf 'awk  : '; printf '6 7\n' | awk '{print $1*$2}'
printf 'grep : '; printf 'foo\nbar\n' | grep bar
printf 'tar  : '; busybox tar --help 2>&1 | head -1
printf 'free : '; df -h "$SHPACK_DIR" 2>/dev/null | tail -1 || true
