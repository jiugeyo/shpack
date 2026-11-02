#!/usr/bin/env bash
# 修复可执行位。GitHub 网页上传 / 某些解压工具会丢掉 +x，导致：
#   ./scripts/fetch-static.sh: Permission denied
# 用法: bash fix-perms.sh
set -u
cd "$(dirname "$0")" || exit 1

chmod +x \
  build.sh pack.sh fix-perms.sh \
  scripts/*.sh \
  tests/*.sh tests/scripts/*.sh \
  examples/*.sh \
  prebuilt/host/shpack \
  dist/shpack-builder dist/shpack-builder-aarch64 2>/dev/null || true

echo "已恢复可执行位。提交时记得把 mode 一起记录："
echo "  git add --chmod=+x build.sh pack.sh fix-perms.sh scripts/*.sh tests/*.sh tests/scripts/*.sh examples/*.sh prebuilt/host/shpack dist/shpack-builder dist/shpack-builder-aarch64"
echo "  git add . && git commit -m 'fix: restore executable bits' && git push"
