# 移动端 (Android App) 优化和新功能

## 修改概述

按照桌面端的优化方案，对移动端进行了相同的改进，并添加了打开示例脚本的功能。

## 1. 性能优化

### 1.1 轮询参数优化 (`lib/src/constants.dart`)

```dart
// 从 2.5s 缩短到 1s
const Duration jobPollInterval = Duration(milliseconds: 1000);

// 从 90s 缩短到 30s
const Duration jobPollTimeout = Duration(seconds: 30);
```

**效果**：
- 响应速度提升 2.5 倍
- 默认超时缩短 3 倍，避免"卡住"感

### 1.2 快速停止函数 (`lib/src/vibe_session.dart`)

添加了 `stopJobAndWait()` 方法：

```dart
Future<({Map<String, dynamic> stopResponse, JobOutcome finalStatus})> stopJobAndWait(
  Bridge bridge, {
  String jobId = '',
  Duration timeout = defaultTimeout,
  Duration waitAfterStop = const Duration(seconds: 2),
}) async {
  // 发送停止请求
  final stopResp = await stopJob(bridge, jobId, timeout: timeout);
  
  // 等待 0.5s 让设备处理
  await Future<void>.delayed(const Duration(milliseconds: 500));
  
  // 轮询 2 秒确认状态
  while (DateTime.now().difference(started) < waitAfterStop) {
    // 查询状态，如果到达终态立即返回
    ...
  }
  
  return (stopResponse: stopResp, finalStatus: lastOutcome);
}
```

**特点**：
- 不做长轮询，最多等待 2-3 秒
- 快速给出反馈，即使 job 未完全停止

### 1.3 AppState 集成快速停止 (`lib/src/app_state.dart`)

```dart
Future<void> stopJob() async {
  _setBusy(true, '停止中');
  log('正在停止 job…');
  try {
    final result = await session.stopJobAndWait(
      bridge,
      jobId: lastJobId,
      timeout: const Duration(seconds: 5),
      waitAfterStop: const Duration(seconds: 2),
    );
    log('job_stop：${result.stopResponse[...]}');
    log('最终状态：${result.finalStatus.status}');
    log(result.finalStatus.describe());
  } finally {
    _setBusy(false);
  }
}
```

## 2. 新功能：打开示例脚本

### 2.1 示例脚本管理 (`lib/src/example_scripts.dart`)

```dart
class ExampleScript {
  const ExampleScript({
    required this.name,
    required this.assetPath,
    this.description = '',
  });
  
  final String name;
  final String assetPath;
  final String description;
}

const List<ExampleScript> exampleScripts = [
  ExampleScript(
    name: '贪吃蛇',
    assetPath: 'assets/examples/贪吃蛇.lua',
    description: '经典贪吃蛇游戏，触摸控制方向',
  ),
  ExampleScript(
    name: '跳跃游戏',
    assetPath: 'assets/examples/跳跃游戏.lua',
    description: '横版跳跃游戏，点击屏幕跳跃',
  ),
  ExampleScript(
    name: '消消乐',
    assetPath: 'assets/examples/消消乐.lua',
    description: '三消游戏，触摸交换方块',
  ),
];
```

### 2.2 资源配置 (`pubspec.yaml`)

```yaml
flutter:
  uses-material-design: true
  assets:
    - assets/catalog.json
    - assets/manifest.json
    - assets/playbooks/
    - assets/lua_modules/
    - assets/examples/  # 新增示例脚本
```

### 2.3 UI 集成 (`lib/src/ui/home_page.dart`)

**Lua 标签页新增按钮**：

```dart
OutlinedButton.icon(
  onPressed: state.busy
      ? null
      : () => _showExamplePicker(context, state),
  icon: const Icon(Icons.folder_open_outlined),
  label: const Text('打开示例'),
),
OutlinedButton.icon(
  onPressed: state.busy || !state.connected
      ? null
      : state.stopJob,
  icon: const Icon(Icons.stop_outlined),
  label: const Text('停止 Job'),
),
```

**示例选择对话框**：

```dart
Future<void> _showExamplePicker(BuildContext context, AppState state) async {
  final selected = await showDialog<ExampleScript>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('选择示例脚本'),
      content: ListView.builder(...),
      // 显示所有可用示例
    ),
  );
  
  if (selected != null) {
    await state.loadExampleFromAssets(selected.assetPath, selected.name);
  }
}
```

## 3. 文件结构

```
mobile_vibe_coding/
├── assets/
│   ├── examples/           # 新增：示例脚本
│   │   ├── 贪吃蛇.lua
│   │   ├── 跳跃游戏.lua
│   │   └── 消消乐.lua
│   ├── catalog.json
│   ├── playbooks/
│   └── lua_modules/
├── lib/
│   ├── src/
│   │   ├── constants.dart          # 修改：优化轮询参数
│   │   ├── vibe_session.dart       # 修改：添加快速停止
│   │   ├── app_state.dart          # 修改：集成快速停止和示例加载
│   │   ├── example_scripts.dart    # 新增：示例脚本定义
│   │   └── ui/
│   │       └── home_page.dart      # 修改：添加UI按钮和对话框
│   └── main.dart
└── pubspec.yaml                    # 修改：添加示例资源
```

## 4. 使用方法

### 4.1 停止 Job

1. 在"Lua"标签页点击"停止 Job"按钮
2. 2-3 秒内得到反馈，显示停止响应和最终状态
3. 在"日志"标签页查看详细信息

### 4.2 打开示例脚本

1. 在"Lua"标签页点击"打开示例"按钮
2. 在弹出的对话框中选择一个示例（贪吃蛇/跳跃游戏/消消乐）
3. 示例脚本自动加载到编辑区
4. 点击"上传并执行"运行脚本

### 4.3 执行流程

```
打开示例 → 编辑（可选）→ 上传并执行 → 查看日志 → 停止 Job（如需）
```

## 5. 效果对比

| 操作 | 修改前 | 修改后 | 改进 |
|------|--------|--------|------|
| 停止 job | 20-90秒 | **2-3秒** | 30倍+ |
| 轮询间隔 | 2.5秒 | **1秒** | 2.5倍 |
| 默认超时 | 90秒 | **30秒** | 3倍缩短 |
| 打开示例 | ❌ 不支持 | ✅ **3个示例** | 新功能 |

## 6. 与桌面端的一致性

移动端的优化完全对标桌面端 (`tools/host_vibe_coding`)：

| 特性 | 桌面端 (Python) | 移动端 (Flutter) | 一致性 |
|------|----------------|-----------------|--------|
| 轮询间隔 | 1.0s | 1.0s | ✅ |
| 默认超时 | 30s | 30s | ✅ |
| 快速停止 | `stop_job_and_wait()` | `stopJobAndWait()` | ✅ |
| 等待时间 | 2秒 | 2秒 | ✅ |
| 示例脚本 | `example_lua/*.lua` | `assets/examples/*.lua` | ✅ |

## 7. 构建和运行

```bash
cd /home/pc/project/esp_claw/tools/mobile_vibe_coding

# 获取依赖
flutter pub get

# 运行（连接设备或模拟器）
flutter run

# 构建 APK
flutter build apk --release

# 构建后的 APK 位于
# build/app/outputs/flutter-apk/app-release.apk
```

## 8. 注意事项

1. **示例脚本更新**：如果修改了 `tools/example_lua/` 中的脚本，需要重新复制到 `assets/examples/`
2. **权限要求**：Android 需要蓝牙和定位权限，首次运行时会自动请求
3. **兼容性**：与桌面端使用相同的协议和常量，确保行为一致

## 9. 后续改进

- [ ] 支持从文件管理器打开 .lua 文件
- [ ] 添加更多示例脚本
- [ ] 支持导出编辑的脚本到本地文件
- [ ] 添加脚本收藏/历史记录功能
