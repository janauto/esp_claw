# ESP-Claw Vibe 移动端 APK 构建说明

## 构建信息

**构建时间**：2026年9月3日 23:09  
**Flutter 版本**：通过 `flutter doctor` 查看  
**构建类型**：Release (发布版)

## APK 文件

### 通用版本（推荐用于分发）
- **文件路径**：`build/app/outputs/flutter-apk/app-release.apk`
- **文件大小**：约 50MB
- **支持架构**：armeabi-v7a, arm64-v8a, x86_64 (全架构)
- **兼容性**：适用于所有 Android 设备

### 分架构版本（更小体积）
正在构建中，将生成：
- `app-armeabi-v7a-release.apk` - 32位 ARM (约 20MB)
- `app-arm64-v8a-release.apk` - 64位 ARM (约 22MB) **推荐大多数现代设备**
- `app-x86_64-release.apk` - x86 64位 (模拟器)

## 功能特性

### 核心功能
✅ BLE 设备扫描和连接  
✅ 自然语言生成 Lua 脚本  
✅ 脚本编辑和上传执行  
✅ 实时日志查看  
✅ 多轮自动迭代  
✅ 快速停止 Job (2-3秒响应)  

### 新增功能
✅ 打开示例脚本（贪吃蛇、跳跃游戏、消消乐）  
✅ 设备事件实时监听  
✅ 优化的轮询性能（1秒间隔）  

## 安装要求

- **Android 版本**：Android 5.0 (API 21) 或更高
- **权限要求**：
  - 蓝牙（BLUETOOTH_SCAN, BLUETOOTH_CONNECT）
  - 位置（用于 BLE 扫描，Android < 12）
- **存储空间**：至少 100MB 可用空间

## 安装步骤

### 方法 1：通过 ADB 安装
```bash
# 连接设备到电脑
adb devices

# 安装 APK
adb install build/app/outputs/flutter-apk/app-release.apk

# 如果已安装旧版本，使用 -r 参数覆盖安装
adb install -r build/app/outputs/flutter-apk/app-release.apk
```

### 方法 2：直接传输到手机
1. 将 APK 文件复制到手机存储
2. 使用文件管理器打开 APK
3. 允许"未知来源"安装（首次需要）
4. 点击安装

### 方法 3：通过微信/QQ 等传输
1. 将 APK 发送到手机
2. 下载 APK 文件
3. 点击安装

## 首次运行

### 1. 权限请求
首次启动时会请求以下权限：
- ✅ 蓝牙扫描
- ✅ 蓝牙连接
- ✅ 位置信息（用于 BLE 扫描）

**请全部允许**，否则无法扫描和连接设备。

### 2. LLM 配置
进入"设置"页面配置：
- Base URL（例如：https://api.openai.com）
- API Key
- 模型名称（例如：gpt-4）
- 最大 Token 数

### 3. 连接设备
1. 确保 ESP-Claw 设备已开机并启用 BLE bridge
2. 在"设备"标签页点击"扫描设备"
3. 选择设备并点击"连接"
4. 查看设备信息确认连接成功

### 4. 快速体验
1. 切换到"Lua"标签页
2. 点击"打开示例"
3. 选择"贪吃蛇"或其他示例
4. 点击"上传并执行"
5. 在"日志"标签页查看执行结果

## 使用指南

### 生成脚本
1. 在"需求"标签页输入需求（例如："做一个打砖块游戏"）
2. 点击"生成 Lua"或"生成并自动迭代"
3. 生成的脚本会出现在"Lua"标签页

### 执行脚本
1. 在"Lua"标签页查看或编辑脚本
2. 点击"上传并执行"
3. 在"日志"标签页查看执行结果
4. 需要停止时点击"停止 Job"

### 查看日志
- 实时日志会自动滚动到最新
- 可以选择复制日志内容
- 点击"清空"清除日志显示

## 性能优化

本版本包含以下优化：
- ⚡ 停止 Job 响应时间：2-3秒（原 90秒）
- ⚡ 轮询间隔：1秒（原 2.5秒）
- ⚡ 默认超时：30秒（原 90秒）
- ⚡ 字体优化：图标字体树摇减少 99.7% (1.6MB → 4.5KB)

## 版本信息

```yaml
name: esp_claw_vibe
version: 0.1.0
sdk: ">=3.4.0 <4.0.0"
```

### 主要依赖
- `flutter_blue_plus: ^1.32.0` - BLE 通信
- `permission_handler: ^11.3.0` - 权限管理
- `http: ^1.2.0` - HTTP 请求
- `shared_preferences: ^2.2.0` - 本地存储

## 故障排查

### 问题 1：无法扫描到设备
**可能原因**：
- 权限未授予
- 设备未开启 BLE
- 设备已被其他手机连接

**解决方法**：
1. 检查权限设置
2. 重启设备蓝牙
3. 断开其他连接

### 问题 2：连接失败
**可能原因**：
- 设备距离太远
- 信号干扰
- 设备繁忙

**解决方法**：
1. 靠近设备
2. 重启 BLE bridge
3. 重新扫描连接

### 问题 3：生成失败
**可能原因**：
- LLM API 配置错误
- 网络问题
- API Key 额度不足

**解决方法**：
1. 检查设置中的配置
2. 确认网络连接
3. 查看日志错误信息

### 问题 4：执行超时
**可能原因**：
- 脚本运行时间过长
- 脚本有死循环

**解决方法**：
1. 点击"停止 Job"强制停止
2. 调大"等待执行结果秒数"
3. 检查脚本是否有 `lvgl.process_events()`

## 构建记录

### 构建命令
```bash
# 获取依赖
flutter pub get

# 构建通用版 APK
flutter build apk --release

# 构建分架构 APK（更小）
flutter build apk --split-per-abi
```

### 构建输出
```
✓ Built build/app/outputs/flutter-apk/app-release.apk (52.1MB)
Font asset tree-shaking: 99.7% reduction (1645KB → 4.5KB)
Build time: 106.4s
```

### 优化建议
- ✅ 已启用 R8 代码压缩
- ✅ 已启用图标字体树摇
- ✅ 使用 Release 模式构建
- 📋 可选：使用 `--split-per-abi` 生成更小的 APK

## 分发建议

### 内部测试
- 使用通用版 APK (`app-release.apk`)
- 通过 ADB 或文件传输安装

### 正式发布
- 使用分架构 APK 减小下载体积
- arm64-v8a 版本适用于大多数现代设备
- 考虑上传到 Google Play Store 或其他应用商店

### 签名（可选）
当前 APK 使用 Flutter 默认的调试签名。正式发布前建议：
1. 生成发布密钥
2. 配置 `android/key.properties`
3. 重新构建并签名

## 相关文档

- 📖 优化说明：`MOBILE_OPTIMIZATION.md`
- 📖 快速参考：`QUICK_REFERENCE.md`
- 📖 总体总结：`../../OPTIMIZATION_SUMMARY.md`

## 支持和反馈

如有问题或建议，请：
1. 查看日志标签页的错误信息
2. 参考故障排查章节
3. 联系开发团队

---

**构建成功** ✅  
**日期**：2026年9月3日  
**版本**：0.1.0
