#!/bin/bash
# 验证 busybox applet 注入与常见命令可用性（脚本本身运行在包内 PATH 下）
set -u
fail=0
check_eq() {
  local name=$1 want=$2 got=$3
  if [ "$got" = "$want" ]; then
    printf 'ok   %-10s %s\n' "$name" "$got"
  else
    printf 'FAIL %-10s want=[%s] got=[%s]\n' "$name" "$want" "$got"
    fail=1
  fi
}

check_eq ls     "/"                    "$(ls -d /)"
check_eq sed    "a+b+c"                "$(printf 'a-b-c\n' | sed 's/-/+/g')"
check_eq awk    "3"                    "$(printf '1 2\n' | awk '{print $1+$2}')"
check_eq grep   "hit"                  "$(printf 'x\nhit\n' | grep hit)"
check_eq sort   "a b"                  "$(printf 'b\na\n' | sort | tr '\n' ' ' | sed 's/ $//')"
check_eq cut    "b"                    "$(printf 'a:b\n' | cut -d: -f2)"
check_eq wc     "1"                    "$(printf 'x\n' | wc -l | tr -d ' ')"
check_eq tar    "ok"                   "$(d=$(mktemp -d); echo ok > "$d/a"; tar -C "$d" -cf "$d/t.tar" a && tar -C "$d" -xf "$d/t.tar" -O a; rm -rf "$d")"
check_eq find   "1"                    "$(d=$(mktemp -d); touch "$d/z"; find "$d" -name z | wc -l | tr -d ' '; rm -rf "$d")"
check_eq xargs  "a b"                  "$(printf 'a\nb\n' | xargs | tr '\n' ' ' | sed 's/ $//')"
check_eq tr     "ABC"                  "$(printf 'abc\n' | tr a-z A-Z)"
check_eq echo   "hello"                "$(echo hello)"
check_eq printf "7"                    "$(printf '%d' 7)"
check_eq test   "yes"                  "$(if [ -d / ]; then echo yes; fi)"
check_eq pwd    "/"                    "$(cd / && pwd)"
check_eq busybox "BusyBox"             "$(busybox | head -1 | cut -d' ' -f1)"
check_eq env    "v"                    "$(FOO=v env | grep '^FOO=' | cut -d= -f2)"
check_eq year4  "1"                    "$(date +%Y | grep -cE '^[0-9]{4}$')"
check_eq uname  "Linux"                "$(uname -s)"
check_eq sleep  "done"                 "$(sleep 0.1; echo done)"
check_eq head   "a"                    "$(printf 'a\nb\n' | head -1)"
check_eq tail   "b"                    "$(printf 'a\nb\n' | tail -1)"
check_eq md5    "900150983cd24fb0d6963f7d28e17f72" "$(printf 'abc' | md5sum | cut -d' ' -f1)"

echo "applet-tests fail=$fail"
exit "$fail"
