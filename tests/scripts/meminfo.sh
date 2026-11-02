#!/bin/bash
# 纯内存模式语义检查
echo "type:$(type -t ls)"
n=$("$SHPACK_BUSYBOX" --list | wc -l)
if [ "$n" -gt 200 ]; then echo "bbcmd:ok"; else echo "bbcmd:fail($n)"; fi
echo "fd:${SHPACK_BUSYBOX_FD:-<none>}"
echo "dir:${SHPACK_DIR:-<none>}"
echo "child:$(bash -c '"$SHPACK_BUSYBOX" md5sum <<<abc' | cut -d' ' -f1)"
echo "child-applet:$(bash -c 'md5sum <<<abc' | cut -d' ' -f1)"
