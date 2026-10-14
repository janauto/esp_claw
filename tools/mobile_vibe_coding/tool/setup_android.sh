#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# 生成 android/ 平台目录，并补上 BLE 权限。
#
# 刻意不在本工程里直接跑 `flutter create .`：那条命令会覆盖 lib/main.dart。
# 这里改成在临时目录里生成一份脚手架，只把 android/ 拷过来，Dart 代码一行不动。
#
#   ./tool/setup_android.sh
#
# 可重复执行：android/ 已存在时只做权限补丁。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

if ! command -v flutter >/dev/null 2>&1; then
  echo "找不到 flutter 命令。先装 Flutter SDK：https://docs.flutter.dev/get-started/install" >&2
  exit 1
fi

PACKAGE_NAME="com.espressif.espclaw.vibe"

if [ ! -d android ]; then
  TMP_DIR="$(mktemp -d)"
  trap 'rm -rf "$TMP_DIR"' EXIT
  echo "正在生成 android/ 脚手架…"
  flutter create \
    --platforms=android \
    --project-name esp_claw_vibe \
    --org "${PACKAGE_NAME%.*}" \
    "$TMP_DIR/scaffold" >/dev/null
  cp -r "$TMP_DIR/scaffold/android" ./android
  echo "已生成 android/（lib/ 与 pubspec.yaml 未被触碰）"
else
  echo "android/ 已存在，跳过生成"
fi

python3 tool/patch_android.py
flutter pub get

echo
echo "完成。打包：flutter build apk --release"
