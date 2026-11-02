#!/bin/bash
# shpack 端到端测试（在 Linux / WSL 中运行）
#   tests/run-tests.sh
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

PACKER=build/host/shpack
STUB=build/x86_64/shpack-stub
BASH_BIN=${SHPACK_BASH:-runtime/x86_64/bash}
BUSYBOX_BIN=${SHPACK_BUSYBOX:-runtime/x86_64/busybox}

PASS=0
FAIL=0
WORK=$(mktemp -d)
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT

# 以前被 SIGKILL 的运行可能留下目录；只要求“本次不新增”
count_leftovers() {
  find /tmp /dev/shm /run/shm -maxdepth 1 -name 'shpack.*' 2>/dev/null | sort -u | wc -l
}
LEFTOVER_BASE=$(count_leftovers)

ok() { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); echo "FAIL: $*"; }
eq() { if [ "$2" = "$3" ]; then ok; else bad "$1: want [$2] got [$3]"; fi; }
contains() { case "$2" in *"$1"*) ok ;; *) bad "$3: 输出中缺少 [$1]，实际为 [$2]" ;; esac; }
not_contains() { case "$2" in *"$1"*) bad "$3: 输出中不应出现 [$1]: [$2]" ;; *) ok ;; esac; }

# ---------------------------------------------------------------------------
# 构建准备
# ---------------------------------------------------------------------------
if [ ! -x "$PACKER" ] || [ ! -f "$STUB" ]; then
  echo "== 构建 packer/stub =="
  make -j"$(nproc)" >/dev/null || { echo "构建失败"; exit 1; }
fi
if [ ! -x "$BASH_BIN" ] || [ ! -x "$BUSYBOX_BIN" ]; then
  echo "== 获取静态 bash/busybox =="
  bash scripts/fetch-static.sh --arch x86_64 --out runtime/x86_64 >/dev/null 2>&1
fi
for f in "$PACKER" "$STUB" "$BASH_BIN" "$BUSYBOX_BIN"; do
  [ -e "$f" ] || { echo "缺少 $f"; exit 1; }
done

pack() { "$PACKER" --stub "$STUB" --bash "$BASH_BIN" --busybox "$BUSYBOX_BIN" "$@"; }

section() { echo; echo "== $* =="; }

# ---------------------------------------------------------------------------
section "0. 编解码单元测试"
# ---------------------------------------------------------------------------
if g++ -std=c++17 -O2 -o "$WORK/codec_test" tests/codec_test.cpp src/lz4.cpp src/sha256.cpp \
     >"$WORK/codec.log" 2>&1; then
  if "$WORK/codec_test" >>"$WORK/codec.log" 2>&1; then ok; else
    bad "codec_test 失败"; cat "$WORK/codec.log"; fi
else
  bad "codec_test 编译失败"; cat "$WORK/codec.log"
fi

# ---------------------------------------------------------------------------
section "1. 打包与 ELF 属性"
# ---------------------------------------------------------------------------
if pack --script examples/hello.sh -o "$WORK/hello" --compress lz4 >"$WORK/pack.log" 2>&1; then ok; else
  bad "打包失败"; cat "$WORK/pack.log"; fi

INFO=$(file -b "$WORK/hello")
contains "ELF 64-bit" "$INFO" "ELF 类型"
contains "statically linked" "$INFO" "静态链接"
if readelf -l "$WORK/hello" 2>/dev/null | grep -q 'INTERP'; then
  bad "不应存在 PT_INTERP"
else
  ok
fi

# ---------------------------------------------------------------------------
section "2. 基本运行 / 参数 / 退出码"
# ---------------------------------------------------------------------------
OUT=$("$WORK/hello" one two 2>&1); RC=$?
eq "退出码" 0 "$RC"
contains "hello from a self-contained ELF" "$OUT" "脚本输出"
contains "args       : 2 -> one two" "$OUT" "参数透传"
contains "mode       : memfd" "$OUT" "默认纯内存模式"
contains "tmp dir    : <none>" "$OUT" "纯内存不落盘"
contains "shell      : 5." "$OUT" "内置 bash"
contains "busybox    : BusyBox" "$OUT" "busybox 命令"

OUT=$("$WORK/hello" --fail 2>&1); RC=$?
eq "未知选项应透传给脚本" 42 "$RC"

OUT=$("$WORK/hello" -- --fail 2>&1); RC=$?
eq "-- 之后透传" 42 "$RC"

# ---------------------------------------------------------------------------
section "3. stdin / 空环境"
# ---------------------------------------------------------------------------
pack --script tests/scripts/stdin.sh -o "$WORK/stdin" >/dev/null 2>&1
OUT=$(printf 'xyz' | "$WORK/stdin" a1 2>&1); RC=$?
eq "stdin 退出码" 0 "$RC"
contains "stdin:[xyz]" "$OUT" "stdin 透传"
contains "arg1:[a1]" "$OUT" "参数"

OUT=$(env -i "$WORK/hello" 2>&1); RC=$?
eq "空环境退出码" 0 "$RC"
contains "hello from" "$OUT" "空环境仍可运行"

# ---------------------------------------------------------------------------
section "4. dir 回退：临时目录选择与清理"
# ---------------------------------------------------------------------------
OUT=$(env TMPDIR=/nonexistent "$WORK/hello" --mode dir 2>&1); RC=$?
eq "TMPDIR 无效时退出码" 0 "$RC"

OUT=$(env TMPDIR= "$WORK/hello" --mode dir 2>&1); RC=$?
eq "TMPDIR 为空时退出码" 0 "$RC"

mkdir -p "$WORK/customtmp"
OUT=$(env SHPACK_TMPDIR="$WORK/customtmp" "$WORK/hello" --mode dir 2>&1); RC=$?
eq "SHPACK_TMPDIR 退出码" 0 "$RC"
contains "tmp dir    : $WORK/customtmp/shpack." "$OUT" "SHPACK_TMPDIR 生效"

DIR=$(printf '%s\n' "$OUT" | sed -n 's/^  tmp dir    : //p')
[ -n "$DIR" ] && ok || bad "dir 模式未输出临时目录"
[ -d "$DIR" ] && bad "运行结束后临时目录仍存在: $DIR" || ok

LEFTOVER=$(count_leftovers)
eq "无新增残留临时目录" "$LEFTOVER_BASE" "$LEFTOVER"

# ---------------------------------------------------------------------------
section "4.5 纯内存模式（默认）"
# ---------------------------------------------------------------------------
OUT=$("$WORK/hello" --mode memfd 2>&1); RC=$?
eq "memfd 退出码" 0 "$RC"
contains "mode       : memfd" "$OUT" "memfd 模式"
contains "tmp dir    : <none>" "$OUT" "SHPACK_DIR 为空"

OUT=$("$WORK/hello" --mode auto 2>&1); RC=$?
eq "auto 退出码" 0 "$RC"
contains "mode       : memfd" "$OUT" "auto 优先纯内存"

# 内存模式不应在任何临时目录留下文件
eq "memfd 不产生临时目录" "$LEFTOVER_BASE" "$(count_leftovers)"
SHMFILES=$(find /dev/shm /run/shm -maxdepth 1 -name 'shpack.*' 2>/dev/null | wc -l)
eq "memfd 不留下 shm 文件" 0 "$SHMFILES"

# applet 以函数形式提供，且 SHPACK_BUSYBOX 可直接当命令用
pack --script tests/scripts/meminfo.sh -o "$WORK/meminfo" >/dev/null 2>&1
OUT=$("$WORK/meminfo" 2>&1); RC=$?
eq "内存语义脚本退出码" 0 "$RC"
contains "type:function" "$OUT" "applet 是函数"
contains "bbcmd:ok" "$OUT" "SHPACK_BUSYBOX 可用"

# ---------------------------------------------------------------------------
section "5. --keep / --extract"
# ---------------------------------------------------------------------------
OUT=$("$WORK/hello" --mode dir --keep 2>&1)
DIR=$(printf '%s\n' "$OUT" | sed -n 's/^  tmp dir    : //p')
[ -d "$DIR" ] && ok || bad "--keep 未保留目录"
[ -x "$DIR/bash" ] && ok || bad "--keep 目录里没有 bash"
rm -rf "$DIR"

OUT=$("$WORK/hello" --extract "$WORK/ex" 2>&1); RC=$?
eq "--extract 退出码" 0 "$RC"
[ -x "$WORK/ex/bash" ] && ok || bad "--extract 缺 bash"
[ -x "$WORK/ex/bin/busybox" ] && ok || bad "--extract 缺 busybox"
OUT=$("$WORK/ex/bash" --version 2>&1); contains "bash" "$OUT" "解出的 bash 可运行"
OUT=$("$WORK/ex/bin/ls" -d / 2>&1); eq "解出的 applet 可运行" "/" "$OUT"

# ---------------------------------------------------------------------------
section "6. 损坏检测"
# ---------------------------------------------------------------------------
cp "$WORK/hello" "$WORK/bad"
SIZE=$(stat -c %s "$WORK/bad")
printf '\xff' | dd of="$WORK/bad" bs=1 seek=$((SIZE / 2)) conv=notrunc status=none
OUT=$("$WORK/bad" 2>&1); RC=$?
eq "负载篡改退出码" 101 "$RC"
contains "SHA-256" "$OUT" "负载篡改提示"

head -c $((SIZE - 256)) "$WORK/hello" >"$WORK/trunc"
chmod +x "$WORK/trunc"
OUT=$("$WORK/trunc" 2>&1); RC=$?
eq "截断退出码" 100 "$RC"

# ---------------------------------------------------------------------------
section "7. 打包器校验"
# ---------------------------------------------------------------------------
if "$PACKER" --stub "$STUB" --bash /bin/true --busybox "$BUSYBOX_BIN" \
     --script examples/hello.sh -o "$WORK/dyn" >"$WORK/dyn.log" 2>&1; then
  bad "动态链接的 bash 应被拒绝"
else
  ok
  contains "动态链接" "$(cat "$WORK/dyn.log")" "动态链接提示"
fi

if pack --stub "$WORK/hello" --script examples/hello.sh -o "$WORK/x" >>"$WORK/dyn.log" 2>&1; then
  bad "已打包的 stub 应被拒绝"
else
  ok
fi

cp "$STUB" "$WORK/stubcopy"
if "$PACKER" --stub "$WORK/stubcopy" --bash "$BASH_BIN" --busybox "$BUSYBOX_BIN" \
     --script examples/hello.sh -o "$WORK/stubcopy" >"$WORK/self.log" 2>&1; then
  bad "输出覆盖输入时应被拒绝"
else
  ok
  contains "输出路径与输入文件相同" "$(cat "$WORK/self.log")" "覆盖输入提示"
fi

# ---------------------------------------------------------------------------
section "8. 压缩选项"
# ---------------------------------------------------------------------------
pack --script examples/hello.sh -o "$WORK/none" --compress none >/dev/null 2>&1
S0=$(stat -c %s "$WORK/none")
S1=$(stat -c %s "$WORK/hello")
[ "$S0" -gt "$S1" ] && ok || bad "none 包应大于 LZ4 包 ($S0 vs $S1)"
"$WORK/none" >/dev/null 2>&1; eq "未压缩包可运行" 0 "$?"

if command -v python3 >/dev/null 2>&1; then
  python3 tests/verify_package.py "$WORK/none" --expect-arch x86_64 >/dev/null 2>&1
  eq "独立格式校验器" 0 "$?"
fi

# ---------------------------------------------------------------------------
section "9. 只读工作目录"
# ---------------------------------------------------------------------------
mkdir -p "$WORK/ro"
chmod 555 "$WORK/ro"
(cd "$WORK/ro" && "$WORK/hello" >/dev/null 2>&1); eq "只读 cwd (memfd)" 0 "$?"
(cd "$WORK/ro" && "$WORK/hello" --mode dir >/dev/null 2>&1); eq "只读 cwd (dir 回退)" 0 "$?"
COUNT=$(find "$WORK/ro" -maxdepth 1 -name 'shpack.*' | wc -l)
eq "只读 cwd 不落盘" 0 "$COUNT"
chmod 755 "$WORK/ro"

# ---------------------------------------------------------------------------
section "10. 并发"
# ---------------------------------------------------------------------------
PIDS=()
for _ in 1 2 3 4 5; do "$WORK/hello" >/dev/null 2>&1 & PIDS+=($!); done
CRASH=0
for p in "${PIDS[@]}"; do wait "$p" || CRASH=1; done
eq "5 个并发实例" 0 "$CRASH"

# ---------------------------------------------------------------------------
section "11. 信号转发与清理"
# ---------------------------------------------------------------------------
pack --script tests/scripts/sig.sh -o "$WORK/sig" --env "SIG_MARK=$WORK/mark" >/dev/null 2>&1
: >"$WORK/sig.out"
"$WORK/sig" --mode dir >"$WORK/sig.out" 2>&1 &
SPID=$!
sleep 1.5
kill -TERM "$SPID" 2>/dev/null
wait "$SPID"; RC=$?
eq "TERM 后退出码" 0 "$RC"
[ -f "$WORK/mark" ] && ok || bad "子脚本未收到 TERM"
SDIR=$(sed -n 's/.*dir=//p' "$WORK/sig.out" | head -1)
if [ -n "$SDIR" ] && [ -e "$SDIR" ]; then bad "信号退出后目录未清理: $SDIR"; else ok; fi

# ---------------------------------------------------------------------------
section "12. 子命令"
# ---------------------------------------------------------------------------
OUT=$("$WORK/hello" --help 2>&1); eq "--help" 0 "$?"
contains "用法:" "$OUT" "help 内容"
OUT=$("$WORK/hello" --version 2>&1); eq "--version" 0 "$?"
contains "shpack" "$OUT" "版本内容"
OUT=$("$WORK/hello" --list 2>&1); eq "--list" 0 "$?"
contains "bash" "$OUT" "list 内容"
"$WORK/hello" --extract >/dev/null 2>&1
eq "--extract 缺参数" 2 "$?"

# ---------------------------------------------------------------------------
section "13. 路径与符号链接"
# ---------------------------------------------------------------------------
mkdir -p "$WORK/with space"
cp "$WORK/hello" "$WORK/with space/app"
OUT=$("$WORK/with space/app" 2>&1); eq "带空格路径" 0 "$?"
ln -s "$WORK/hello" "$WORK/link"
OUT=$("$WORK/link" 2>&1); eq "符号链接" 0 "$?"

# ---------------------------------------------------------------------------
section "14. 大脚本 / bash 选项 / PATH 模式"
# ---------------------------------------------------------------------------
{
  echo '#!/bin/bash'
  for i in $(seq 1 8000); do echo "# filler $i"; done
  echo 'echo BIG-OK'
} >"$WORK/big.sh"
chmod 755 "$WORK/big.sh"
pack --script "$WORK/big.sh" -o "$WORK/big" --compress lz4 >/dev/null 2>&1
OUT=$("$WORK/big" 2>&1); eq "大脚本退出码" 0 "$?"
contains "BIG-OK" "$OUT" "大脚本输出"

pack --script examples/hello.sh -o "$WORK/syntax" --bash-opt -n >/dev/null 2>&1
OUT=$("$WORK/syntax" 2>&1); eq "bash -n 退出码" 0 "$?"
eq "bash -n 不执行脚本" "" "$OUT"

pack --script tests/scripts/applets.sh -o "$WORK/applets" >/dev/null 2>&1
OUT=$("$WORK/applets" 2>&1); RC=$?
eq "applet 测试退出码 (memfd)" 0 "$RC"
contains "applet-tests fail=0" "$OUT" "applet 全部可用 (memfd)"

OUT=$("$WORK/applets" --mode dir 2>&1); RC=$?
eq "applet 测试退出码 (dir)" 0 "$RC"
contains "applet-tests fail=0" "$OUT" "applet 全部可用 (dir)"

# ---------------------------------------------------------------------------
section "15. 名称 / 无预检"
# ---------------------------------------------------------------------------
pack --script examples/hello.sh -o "$WORK/named" --name mytool >/dev/null 2>&1
OUT=$("$WORK/named" --list 2>&1)
contains "名称    : mytool" "$OUT" "--name 生效"
OUT=$("$WORK/named" 2>&1)
contains "script     : " "$OUT" "脚本路径"

OUT=$("$WORK/hello" --no-preflight 2>&1); eq "--no-preflight" 0 "$?"

cp examples/hello.sh "$WORK/mode640.sh"
chmod 640 "$WORK/mode640.sh"
pack --script "$WORK/mode640.sh" -o "$WORK/mode640" >/dev/null 2>&1
eq "输出权限跟随脚本" 640 "$(stat -c %a "$WORK/mode640")"

# ---------------------------------------------------------------------------
echo
echo "==========================================="
echo "通过: $PASS   失败: $FAIL"
echo "==========================================="
if [ "$FAIL" -eq 0 ]; then
  exit 0
else
  exit 1
fi