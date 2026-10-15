#!/bin/zsh
# 磁吸背夹 1.0 固件构建（IDF 6.1，esp32_s31_korvo1）。用法：build.sh [日志文件]
set -u
LOG=${1:-/tmp/backclip-1.0-build.log}
export IDF_PATH=~/esp/idf-mosaico-v6.1
export IDF_TOOLS_PATH=~/esp/tools-mosaico-v6.1
source $IDF_PATH/export.sh >/dev/null 2>&1
cd ~/.cache/backclip-web/esp_claw-voice/application/edge_agent
idf.py build > "$LOG" 2>&1
rc=$?
echo "BUILD_EXIT $rc" >> "$LOG"
exit $rc
