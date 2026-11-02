#!/system/bin/sh
# 在 Android 设备上对 shpack 产物做冒烟测试（POSIX sh / toybox 可运行）
#
#   adb push build/hello-arm64 /data/local/tmp/hello
#   adb push tests/android-smoke.sh /data/local/tmp/
#   adb shell sh /data/local/tmp/android-smoke.sh /data/local/tmp/hello
set -u

APP=${1:-}
if [ -z "$APP" ] || [ ! -x "$APP" ]; then
  echo "用法: sh $0 /path/to/executable" >&2
  exit 2
fi

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; }

TMPBASE=${TMPDIR:-/data/local/tmp}
EXDIR=$TMPBASE/shpack-smoke-extract

echo "== 1. 版本与清单 =="
if "$APP" --version >"$TMPBASE/shpack-smoke-v" 2>&1; then ok "--version"; else bad "--version"; fi
cat "$TMPBASE/shpack-smoke-v" 2>/dev/null

if "$APP" --list >"$TMPBASE/shpack-smoke-l" 2>&1; then ok "--list"; else bad "--list"; fi
if grep -q 'bash' "$TMPBASE/shpack-smoke-l" 2>/dev/null; then ok "清单含 bash"; else bad "清单缺 bash"; fi

echo "== 2. 运行与退出码 =="
BEFORE=$(ls -d "$TMPBASE"/shpack.* 2>/dev/null | wc -l | tr -d ' ')
OUT=$("$APP" a b 2>&1)
RC=$?
if [ "$RC" -eq 0 ]; then ok "运行退出码 0"; else bad "运行退出码 $RC"; fi
echo "$OUT" | grep -E 'mode       :|tmp dir    :' || true
case "$OUT" in
  *"mode       : memfd"*) ok "纯内存模式" ;;
  *"mode       : dir"*) echo "note: 当前回退到 dir 模式（设备/SELinux 限制）" ;;
esac

"$APP" --fail >/dev/null 2>&1
RC=$?
if [ "$RC" -eq 42 ]; then ok "退出码透传 42"; else bad "退出码透传: $RC"; fi

echo "== 3. 解包与 applet =="
rm -rf "$EXDIR"
if "$APP" --extract "$EXDIR" >/dev/null 2>&1; then ok "--extract"; else bad "--extract"; fi
if [ -x "$EXDIR/bash" ]; then ok "解出 bash"; else bad "缺 bash"; fi
if [ -x "$EXDIR/bin/busybox" ]; then ok "解出 busybox"; else bad "缺 busybox"; fi

N=$("$EXDIR/bin/busybox" --list 2>/dev/null | wc -l | tr -d ' ')
if [ "$N" -gt 200 ]; then ok "applet 数量 $N"; else bad "applet 数量异常: $N"; fi

if "$EXDIR/bash" -c 'printf x | sed s/x/y/ | grep -q y'; then ok "解出的 bash+applet 可用"; else bad "解出的 bash 不可用"; fi

echo "== 4. 临时目录清理 =="
AFTER=$(ls -d "$TMPBASE"/shpack.* 2>/dev/null | wc -l | tr -d ' ')
if [ "$BEFORE" -eq "$AFTER" ]; then ok "无残留临时目录"; else bad "残留目录 ($BEFORE -> $AFTER)"; fi

rm -rf "$EXDIR" "$TMPBASE/shpack-smoke-v" "$TMPBASE/shpack-smoke-l"

echo
echo "通过=$PASS 失败=$FAIL"
[ "$FAIL" -eq 0 ]
