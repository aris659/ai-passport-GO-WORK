#!/bin/bash
# Docker 卷内构建（规避 Windows 绑定挂载小文件 I/O 慢的问题）
set -e

# 首次运行：完整播种（含已下载的 managed_components，免重下 179MB LVGL）
if [ ! -f /work/CMakeLists.txt ]; then
  echo "=== Seeding volume from bind mount ==="
  (cd /src && tar cf - .) | (cd /work && tar xf -)
fi

# 增量同步源码（跳过依赖、构建产物、发布物）
echo "=== Syncing source ==="
(cd /src && tar cf - \
  --exclude=./build --exclude=./managed_components \
  --exclude=./dependencies.lock --exclude=./releases --exclude=.git .) \
  | (cd /work && tar xf -)

cd /work
if [ ! -f sdkconfig ]; then
  idf.py set-target esp32c3
fi

echo "=== Building ==="
idf.py build

echo "=== Merging binary ==="
python -m esptool --chip esp32c3 merge_bin -o build/pomodoro-merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/FoloToy-AI-Passport.bin

echo "=== Copying artifacts back ==="
# RELEASE_NAME 可覆盖；默认写当前开发版本。候选/修复固件用:
#   RELEASE_NAME=whiplash-esp32c3-v1.3.0-batteryfix-merged.bin bash tools/docker_build.sh
RELEASE_NAME="${RELEASE_NAME:-whiplash-esp32c3-v1.3.0-merged.bin}"
mkdir -p /src/build
cp build/FoloToy-AI-Passport.bin /src/build/
cp build/pomodoro-merged.bin /src/build/
cp build/pomodoro-merged.bin "/src/releases/${RELEASE_NAME}"
ls -la build/*.bin
echo "BUILD_ARTIFACTS_DONE"
