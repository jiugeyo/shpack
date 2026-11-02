#!/bin/bash
# 单文件 builder 测试：CLI、4 架构产物、权限、交互式（一次只打一个包）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

BUILDER=${SHPACK_BUILDER:-dist/shpack-builder}
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); echo "FAIL: $*"; }
contains() { case "$2" in *"$1"*) ok ;; *) bad "$3: 缺少 [$1]" ;; esac; }
not_contains() { case "$2" in *"$1"*) bad "$3: 不应出现 [$1]" ;; *) ok ;; esac; }

if [ ! -x "$BUILDER" ]; then
  echo "缺少 $BUILDER，先生成：scripts/make-builder.sh" >&2
  exit 1
fi

ARCHES=$(printf '%s\n' "$("$BUILDER" list 2>/dev/null)")
for a in x86_64 aarch64 armv7 i386; do
  contains "$a" "$ARCHES" "内嵌架构 $a"
done
contains "bundle build" "$ARCHES" "bundle 信息"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for a in x86_64 aarch64 armv7 i386; do
  if "$BUILDER" -s examples/hello.sh -o "$WORK/pkg-$a" -a "$a" --compress lz4 \
       >"$WORK/log-$a" 2>&1; then
    ok
  else
    bad "构建 $a 失败"; cat "$WORK/log-$a"; continue
  fi
  if python3 tests/verify_package.py "$WORK/pkg-$a" --expect-arch "$a" >/dev/null 2>&1; then
    ok
  else
    bad "$a 产物格式校验失败"
  fi
done

# x86_64 产物真实执行
"$WORK/pkg-x86_64" >/dev/null 2>&1
[ $? -eq 0 ] && ok || bad "x86_64 产物运行失败"

# extract
"$BUILDER" extract --arch armv7 --out "$WORK/ex" >/dev/null 2>&1
[ -x "$WORK/ex/bash" ] && [ -x "$WORK/ex/busybox" ] && ok || bad "extract 失败"
file -b "$WORK/ex/busybox" | grep -q 'ARM' && ok || bad "extract 架构不对"

# 输出权限跟随输入
cp examples/hello.sh "$WORK/m640.sh"
chmod 640 "$WORK/m640.sh"
"$BUILDER" -s "$WORK/m640.sh" -o "$WORK/m640" -a x86_64 >/dev/null 2>&1
[ "$(stat -c %a "$WORK/m640" 2>/dev/null)" = "640" ] && ok || bad "输出权限未跟随输入"

# 交互式向导（PTY）：默认输出名应为 hello.sh；打包一次后直接退出
TMPD=$(mktemp -d)
(
  cd "$TMPD" || exit 1
  printf "$ROOT/examples/hello.sh\n1\n\n\n\n\n\n\ny\n" | \
    script -qec "$ROOT/$BUILDER" /dev/null >"$WORK/interactive.log" 2>&1
)
if [ -x "$TMPD/hello.sh" ] && "$TMPD/hello.sh" >/dev/null 2>&1; then
  ok
else
  bad "交互式打包失败（默认输出名）"; tail -5 "$WORK/interactive.log"
fi
not_contains "再打一个" "$(cat "$WORK/interactive.log")" "不再询问继续打包"
not_contains "adb push" "$(cat "$WORK/interactive.log")" "末尾不出现推送提示"
rm -rf "$TMPD"

echo
echo "builder 测试: 通过 $PASS, 失败 $FAIL"
[ "$FAIL" -eq 0 ]
