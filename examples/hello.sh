#!/usr/bin/env bash
# 示例：展示内置 bash / busybox 与参数透传
set -euo pipefail

echo "hello from a self-contained ELF"
echo "  shell      : $BASH_VERSION"
echo "  script     : $0"
echo "  args       : $# -> $*"
echo "  busybox    : $(busybox | head -1)"
echo "  uname      : $(uname -sm)"
echo "  ls (busybox): $(ls -d / | head -1)"
echo "  sed        : $(printf 'a-b-c\n' | sed 's/-/+/g')"
echo "  awk        : $(printf '1 2\n' | awk '{print $1+$2}')"
echo "  self       : $SHPACK_SELF"
echo "  mode       : $SHPACK_MODE"
echo "  tmp dir    : ${SHPACK_DIR:-<none>}"

if [ "${1:-}" = "--fail" ]; then
  echo "exit 42 on purpose" >&2
  exit 42
fi
