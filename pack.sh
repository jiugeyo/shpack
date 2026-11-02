#!/usr/bin/env bash
# 便捷打包脚本：自动选择 stub 与静态 bash/busybox。
#
#   ./pack.sh -s deploy.sh -o deploy
#   ./pack.sh --arch aarch64 -s deploy.sh -o deploy-arm64 --compress lz4
#   ./pack.sh -s deploy.sh -- --name 会被当作打包参数，别这样写
#
# 其余未识别参数原样传给 build/host/shpack。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
ARCH=$(uname -m)
SCRIPT=""
OUTPUT=""
NAME=""
STUB=""
BASH_BIN=""
BUSYBOX_BIN=""
PASSTHROUGH=()

need_val() {
  if [ $# -lt 2 ]; then echo "参数 $1 缺少取值" >&2; exit 2; fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    -s|--script) need_val "$@"; SCRIPT=$2; shift 2;;
    -o|--output) need_val "$@"; OUTPUT=$2; shift 2;;
    -n|--name) need_val "$@"; NAME=$2; shift 2;;
    -a|--arch) need_val "$@"; ARCH=$2; shift 2;;
    --stub) need_val "$@"; STUB=$2; shift 2;;
    --bash) need_val "$@"; BASH_BIN=$2; shift 2;;
    --busybox) need_val "$@"; BUSYBOX_BIN=$2; shift 2;;
    --compress|--mode|--path-mode|--env|--bash-opt) need_val "$@"; PASSTHROUGH+=("$1" "$2"); shift 2;;
    -h|--help)
      sed -n '2,9p' "$0"
      echo
      echo "可用参数: -s/--script -o/--output -n/--name -a/--arch --stub --bash --busybox"
      echo "          --compress --mode --path-mode --env --bash-opt （其余见 shpack --help）"
      exit 0;;
    *) echo "未知参数: $1" >&2; exit 2;;
  esac
done

if [ -z "$SCRIPT" ]; then
  echo "用法: $0 -s SCRIPT [-o OUTPUT] [--arch aarch64]" >&2
  exit 2
fi
[ -f "$SCRIPT" ] || { echo "脚本不存在: $SCRIPT" >&2; exit 1; }
OUTPUT=${OUTPUT:-$(basename "$SCRIPT" .sh)}
if [ -z "$STUB" ]; then
  if [ -f "$ROOT/build/$ARCH/shpack-stub" ]; then
    STUB=$ROOT/build/$ARCH/shpack-stub
  else
    STUB=$ROOT/prebuilt/stub/$ARCH/shpack-stub
  fi
fi
# 优先用本地新构建/新下载的 runtime，其次用仓库自带的 prebuilt
if [ -z "$BASH_BIN" ]; then
  if [ -f "$ROOT/runtime/$ARCH/bash" ]; then
    BASH_BIN=$ROOT/runtime/$ARCH/bash
  else
    BASH_BIN=$ROOT/prebuilt/$ARCH/bash
  fi
fi
if [ -z "$BUSYBOX_BIN" ]; then
  if [ -f "$ROOT/runtime/$ARCH/busybox" ]; then
    BUSYBOX_BIN=$ROOT/runtime/$ARCH/busybox
  else
    BUSYBOX_BIN=$ROOT/prebuilt/$ARCH/busybox
  fi
fi

if [ -z "${PACKER:-}" ]; then
  if [ -x "$ROOT/build/host/shpack" ]; then
    PACKER=$ROOT/build/host/shpack
  else
    PACKER=$ROOT/prebuilt/host/shpack
  fi
fi
# 网页上传/解压可能丢掉可执行位，这里兜底
if [ -f "$PACKER" ] && [ ! -x "$PACKER" ]; then
  chmod +x "$PACKER" 2>/dev/null || true
fi
[ -x "$PACKER" ] || { echo "缺少 packer，请运行 ./build.sh（或使用仓库自带的 prebuilt/host/shpack）" >&2; exit 1; }
[ -f "$STUB" ] || { echo "缺少 stub: $STUB，请运行 ./build.sh --arch $ARCH" >&2; exit 1; }
[ -f "$BASH_BIN" ] || { echo "缺少静态 bash: $BASH_BIN，请运行 scripts/fetch-static.sh --arch $ARCH" >&2; exit 1; }
[ -f "$BUSYBOX_BIN" ] || { echo "缺少静态 busybox: $BUSYBOX_BIN，请运行 scripts/fetch-static.sh --arch $ARCH" >&2; exit 1; }

ARGS=(--stub "$STUB" --bash "$BASH_BIN" --busybox "$BUSYBOX_BIN" --script "$SCRIPT" -o "$OUTPUT")
[ -n "$NAME" ] && ARGS+=(--name "$NAME")
exec "$PACKER" "${ARGS[@]}" "${PASSTHROUGH[@]}"
