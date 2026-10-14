#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""给 flutter create 生成的 android/ 补上 BLE 权限和最低 SDK。

flutter create 出来的清单只有 INTERNET，扫不到也连不上 BLE。这里按 Android
版本分段声明：

- Android 12（API 31）起用 BLUETOOTH_SCAN / BLUETOOTH_CONNECT；
  neverForLocation 表示不用扫描结果推位置，否则还要额外申请定位权限。
- Android 11 及以下用旧的 BLUETOOTH / BLUETOOTH_ADMIN，且必须有定位权限
  才能拿到扫描结果，这是系统限制，不是应用可以绕过的。

可重复执行：已经打过的补丁会被跳过。
"""

from __future__ import annotations

import re
import sys
import time
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = PROJECT_ROOT / "android" / "app" / "src" / "main" / "AndroidManifest.xml"

PERMISSION_MARKER = "android.permission.BLUETOOTH_SCAN"

PERMISSIONS = """    <uses-permission android:name="android.permission.INTERNET" />

    <!-- Android 12+ -->
    <uses-permission
        android:name="android.permission.BLUETOOTH_SCAN"
        android:usesPermissionFlags="neverForLocation" />
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />

    <!-- Android 11 及以下：旧权限 + 定位（系统要求，否则扫不到结果） -->
    <uses-permission
        android:name="android.permission.BLUETOOTH"
        android:maxSdkVersion="30" />
    <uses-permission
        android:name="android.permission.BLUETOOTH_ADMIN"
        android:maxSdkVersion="30" />
    <uses-permission
        android:name="android.permission.ACCESS_FINE_LOCATION"
        android:maxSdkVersion="30" />

    <uses-feature android:name="android.hardware.bluetooth_le" android:required="true" />
"""

#: flutter_blue_plus 需要的最低 SDK。低于 21 编译不过。
MIN_SDK = 21

#: 官方 Gradle 源慢到不可用时的替代。实测这个镜像同内容、快三个数量级。
GRADLE_MIRROR_HOST = "mirrors.cloud.tencent.com"

#: 低于这个物理内存就下调 Gradle 堆。模板默认 -Xmx8G，4G 机器上必被 OOM killer 杀。
LOW_MEMORY_THRESHOLD_MB = 4096

#: 写进 gradle.properties 的幂等标记。
LOW_MEMORY_MARKER = "低内存构建参数"


def patch_manifest() -> bool:
    if not MANIFEST.exists():
        raise SystemExit(f"找不到清单文件：{MANIFEST}\n先跑 tool/setup_android.sh 生成 android/")

    text = MANIFEST.read_text(encoding="utf-8")
    if PERMISSION_MARKER in text:
        print("清单已包含 BLE 权限，跳过")
        return False

    # 已有的 INTERNET 声明连同我们要插入的块一起替换，避免重复声明。
    existing = re.search(
        r'[ \t]*<uses-permission android:name="android\.permission\.INTERNET"\s*/>\n',
        text,
    )
    if existing:
        text = text[: existing.start()] + PERMISSIONS + text[existing.end():]
    else:
        # 没有 INTERNET 就插在 <application 之前。
        marker = re.search(r"[ \t]*<application", text)
        if not marker:
            raise SystemExit("清单里找不到 <application>，格式和预期不符，请手动添加权限")
        text = text[: marker.start()] + PERMISSIONS + "\n" + text[marker.start():]

    MANIFEST.write_text(text, encoding="utf-8")
    print("已写入 BLE 权限")
    return True


def patch_min_sdk() -> bool:
    """把 minSdk 抬到 flutter_blue_plus 能编译的版本。"""
    candidates = [
        PROJECT_ROOT / "android" / "app" / "build.gradle.kts",
        PROJECT_ROOT / "android" / "app" / "build.gradle",
    ]
    gradle = next((path for path in candidates if path.exists()), None)
    if gradle is None:
        print("警告：找不到 app 的 build.gradle，minSdk 需手动确认 >= 21")
        return False

    text = gradle.read_text(encoding="utf-8")
    # 新模板用 flutter.minSdkVersion 占位，旧模板直接写数字。
    patched, count = re.subn(
        r"minSdk(?:Version)?\s*=?\s*(?:flutter\.minSdkVersion|\d+)",
        lambda m: m.group(0).split("=")[0].rstrip() +
        (" = " if "=" in m.group(0) else " ") + str(MIN_SDK),
        text,
    )
    if count == 0:
        print("警告：没匹配到 minSdk 声明，请手动确认 >= 21")
        return False
    if patched == text:
        print(f"minSdk 已是 {MIN_SDK}，跳过")
        return False

    gradle.write_text(patched, encoding="utf-8")
    print(f"已把 {gradle.name} 的 minSdk 设为 {MIN_SDK}")
    return True


def patch_gradle_distribution() -> bool:
    """必要时把 Gradle 发行版下载地址换成镜像。

    services.gradle.org 在国内经常是几百 B/s（实测 220 B/s，等同卡死），
    而 gradle-*-all.zip 有 250 MB 左右，不换镜像第一次构建基本下不完。

    只在官方源确实慢的时候才改：网络没问题的机器不该被无谓地指向第三方镜像。
    """
    props = PROJECT_ROOT / "android" / "gradle" / "wrapper" / "gradle-wrapper.properties"
    if not props.exists():
        print("警告：找不到 gradle-wrapper.properties，跳过镜像检查")
        return False

    text = props.read_text(encoding="utf-8")
    if GRADLE_MIRROR_HOST in text:
        print("Gradle 发行版已指向镜像，跳过")
        return False
    if "services.gradle.org" not in text:
        return False

    match = re.search(r"gradle-([\d.]+)-(all|bin)\.zip", text)
    if not match:
        return False
    archive = match.group(0)

    if _official_gradle_is_fast(archive):
        print("官方 Gradle 源速度正常，保持默认")
        return False

    mirror_url = f"https\\://{GRADLE_MIRROR_HOST}/gradle/{archive}"
    text = re.sub(
        r"distributionUrl=.*",
        f"distributionUrl={mirror_url}",
        text,
    )
    props.write_text(text, encoding="utf-8")
    print(f"官方 Gradle 源过慢，已切到镜像：{GRADLE_MIRROR_HOST}")
    return True


def _official_gradle_is_fast(archive: str) -> bool:
    """抓 1 MB 试速。抓不动就认为官方源不可用。"""
    url = f"https://services.gradle.org/distributions/{archive}"
    request = urllib.request.Request(url, headers={"Range": "bytes=0-1048575"})
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            read = len(response.read())
    except Exception:
        return False
    elapsed = max(time.monotonic() - started, 0.001)
    speed = read / elapsed
    print(f"官方 Gradle 源实测 {speed / 1024:.0f} KB/s")
    #: 低于这个速度，250 MB 要下超过 20 分钟，不如直接换镜像。
    return speed > 200 * 1024


def _total_ram_mb() -> int | None:
    """读 /proc/meminfo 拿物理内存。非 Linux 或读不到就返回 None。"""
    try:
        for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("MemTotal:"):
                return int(line.split()[1]) // 1024
    except Exception:
        return None
    return None


def patch_gradle_memory() -> bool:
    """内存小的机器上，把 Gradle 的 JVM 堆从模板默认值调下来。

    Flutter 模板固定写 -Xmx8G。物理内存不够时 JVM 会向内核要到超出可用量，
    然后被 OOM killer 杀掉，构建侧只看到一句
    "Gradle build daemon disappeared unexpectedly"，看不出是内存问题。

    只在物理内存低于阈值时才动：内存充足的机器保留默认值，构建更快。
    """
    props = PROJECT_ROOT / "android" / "gradle.properties"
    if not props.exists():
        print("警告：找不到 android/gradle.properties，跳过内存调整")
        return False

    text = props.read_text(encoding="utf-8")
    if LOW_MEMORY_MARKER in text:
        print("Gradle 内存参数已按小内存调过，跳过")
        return False

    ram = _total_ram_mb()
    if ram is None:
        print("读不到物理内存大小，保持 Gradle 默认内存参数")
        return False
    if ram >= LOW_MEMORY_THRESHOLD_MB:
        print(f"物理内存 {ram} MB，够用，保持 Gradle 默认内存参数")
        return False

    # 给内核和其他进程留出余量，剩下的一半给堆。堆之外 JVM 还要吃
    # metaspace、code cache、线程栈，堆开到接近物理内存必然被杀。
    heap = max(512, int(ram * 0.5))
    metaspace = max(256, heap // 2)

    block = (
        f"# {LOW_MEMORY_MARKER}：本机物理内存 {ram} MB，模板默认的 -Xmx8G 会被内核\n"
        "# OOM killer 杀掉（dmesg 里是 \"Out of memory: Killed process (java)\"），\n"
        "# 构建侧表现为 \"Gradle build daemon disappeared unexpectedly\"。\n"
        f"org.gradle.jvmargs=-Xmx{heap}m -XX:MaxMetaspaceSize={metaspace}m "
        "-XX:ReservedCodeCacheSize=192m\n"
        "# 串行执行，避免多个 worker 各起一个 JVM 把内存吃穿。\n"
        "org.gradle.parallel=false\n"
        "org.gradle.workers.max=1\n"
        "org.gradle.daemon=false\n"
        "# Kotlin 编译默认另开一个 JVM，这个内存下必须并进主进程。\n"
        "kotlin.compiler.execution.strategy=in-process\n"
        "kotlin.incremental=false\n"
    )

    # 替换掉模板那行 jvmargs，避免两行 jvmargs 并存（后一行会静默覆盖前一行）。
    text, count = re.subn(r"^org\.gradle\.jvmargs=.*\n", block, text, count=1,
                          flags=re.MULTILINE)
    if count == 0:
        text = block + text

    props.write_text(text, encoding="utf-8")
    print(f"物理内存 {ram} MB，已把 Gradle 堆设为 {heap}m 并改为串行构建")
    return True


def main() -> None:
    patch_manifest()
    patch_min_sdk()
    patch_gradle_distribution()
    patch_gradle_memory()


if __name__ == "__main__":
    sys.exit(main())
