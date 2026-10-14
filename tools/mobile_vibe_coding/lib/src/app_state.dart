// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';

import 'ble_bridge.dart';
import 'constants.dart';
import 'job_outcome.dart';
import 'llm_client.dart';
import 'llm_config.dart';
import 'module_catalog.dart';
import 'playbook_registry.dart';
import 'task_planner.dart';
import 'vibe_session.dart';

/// 把 AppState 递给子树。用 InheritedNotifier 而不是引第三方状态管理：
/// 这个应用只有一份全局状态，多一个依赖不划算。
class AppStateScope extends InheritedNotifier<AppState> {
  const AppStateScope({super.key, required AppState state, required super.child})
      : super(notifier: state);

  static AppState of(BuildContext context) {
    final scope = context.dependOnInheritedWidgetOfExactType<AppStateScope>();
    assert(scope?.notifier != null, '找不到 AppStateScope');
    return scope!.notifier!;
  }
}

/// 全局状态。BLE 与 LLM 都是异步的，界面只读这里的字段。
class AppState extends ChangeNotifier {
  final ConfigStore _store = ConfigStore();
  final LlmClient _llm = LlmClient();

  ModuleCatalog? _catalog;
  PlaybookRegistry? _registry;
  VibeSession? _session;

  LlmConfig config = const LlmConfig();
  bool ready = false;
  String bootError = '';

  List<ScannedDevice> devices = const [];
  bool scanning = false;

  BleBridge? _bridge;
  bool connecting = false;
  String deviceLabel = '';
  String deviceInfo = '';

  /// 当前编辑区里的脚本。生成后可手改再上传。
  final TextEditingController scriptController = TextEditingController();
  final TextEditingController promptController = TextEditingController();

  /// 执行日志。界面倒序展示最新的在下面。
  final List<String> logLines = [];

  bool busy = false;
  String busyLabel = '';
  TaskPlan? lastPlan;
  String lastJobId = '';

  /// 迭代轮数与轮询等待秒数，界面上可调。
  int maxAttempts = defaultMaxAttempts;
  int pollTimeoutS = jobPollTimeout.inSeconds;

  bool get connected => _bridge?.isConnected ?? false;

  Future<void> bootstrap() async {
    try {
      config = await _store.loadConfig();
      _catalog = await ModuleCatalog.load();
      _registry = await PlaybookRegistry.load();
      _session = VibeSession(
        catalog: _catalog!,
        registry: _registry!,
        llm: _llm,
      );
      ready = true;
      log('已载入 ${_catalog!.modules.length} 个模块条目、'
          '${_registry!.playbooks.length} 份 playbook');
    } on Object catch (err) {
      bootError = '$err';
    }
    notifyListeners();
  }

  void log(String message) {
    if (message.trim().isEmpty) return;
    logLines.add(message);
    // 日志无上限会让长任务把内存吃满，也让界面滚动变慢。
    if (logLines.length > 600) {
      logLines.removeRange(0, logLines.length - 500);
    }
    notifyListeners();
  }

  void clearLog() {
    logLines.clear();
    notifyListeners();
  }

  Future<void> saveConfig(LlmConfig next) async {
    config = next;
    await _store.saveConfig(next);
    log('已保存 LLM 配置：${providerLabel(next.provider)} / ${next.model}');
    notifyListeners();
  }

  void _setBusy(bool value, [String label = '']) {
    busy = value;
    busyLabel = label;
    notifyListeners();
  }

  /// Android 12+ 把扫描和连接拆成独立的运行时权限，两个都要拿到。
  /// 低版本还需要定位权限才能扫到 BLE 设备。
  Future<bool> ensurePermissions() async {
    final results = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();

    final scanOk = results[Permission.bluetoothScan]?.isGranted ?? false;
    final connectOk = results[Permission.bluetoothConnect]?.isGranted ?? false;
    // 老版本 Android 上 bluetoothScan/Connect 恒为 granted，真正的门槛是定位。
    final locationOk = results[Permission.locationWhenInUse]?.isGranted ?? false;

    if (scanOk && connectOk) return true;
    if (locationOk) return true;
    log('蓝牙权限被拒绝，无法扫描设备。请在系统设置里授予「附近的设备」权限。');
    return false;
  }

  Future<void> scan() async {
    if (scanning) return;
    if (!await ensurePermissions()) return;
    scanning = true;
    devices = const [];
    notifyListeners();
    log('正在扫描 ESP-Claw bridge…');
    try {
      devices = await BleBridge.scan();
      log(devices.isEmpty
          ? '没有扫到设备。确认设备已开机、BLE bridge 已启用且不在被其它手机连着。'
          : '扫到 ${devices.length} 个候选设备');
    } on Object catch (err) {
      log('扫描失败：$err');
    } finally {
      scanning = false;
      notifyListeners();
    }
  }

  Future<void> connect(ScannedDevice target) async {
    if (connecting) return;
    if (!await ensurePermissions()) return;
    connecting = true;
    notifyListeners();
    log('正在连接 ${target.name}…');
    try {
      final bridge = BleBridge(
        onLog: log,
        onDeviceEvent: (frame) {
          // 设备端 job 进终态时主动推一帧。轮询仍是主路径，这里只记录，
          // 这样旧固件（不推送）也能正常工作。
          log('设备推送：${frame['data'] ?? frame}');
        },
      );
      await bridge.connect(target.device);
      _bridge = bridge;
      deviceLabel = bridge.deviceLabel;
      notifyListeners();
      await refreshDeviceInfo();
    } on Object catch (err) {
      log('连接失败：$err');
      _bridge = null;
      deviceLabel = '';
    } finally {
      connecting = false;
      notifyListeners();
    }
  }

  Future<void> disconnect() async {
    final bridge = _bridge;
    _bridge = null;
    deviceLabel = '';
    deviceInfo = '';
    notifyListeners();
    if (bridge != null) {
      await bridge.disconnect();
      log('已断开连接');
    }
  }

  Future<void> refreshDeviceInfo() async {
    final bridge = _bridge;
    final session = _session;
    if (bridge == null || session == null) {
      log('请先连接设备');
      return;
    }
    try {
      deviceInfo = await session.fetchDeviceInfo(bridge);
      log('已获取设备信息：\n$deviceInfo');
    } on Object catch (err) {
      log('获取设备信息失败：$err');
    }
    notifyListeners();
  }

  /// 只跑规划阶段，显示会导入哪些 README。不连设备也能用。
  Future<void> previewPlan() async {
    final session = _session;
    if (session == null) return;
    final prompt = promptController.text.trim();
    if (prompt.isEmpty) {
      log('请先输入需求');
      return;
    }
    _setBusy(true, '规划中');
    log('--- 查看模块选择 ---');
    try {
      final plan = await session.plan(config, prompt, deviceInfo: deviceInfo);
      lastPlan = plan;
      // 规划本身不载入 README，这里补一次，让用户看到实际会进上下文的文档。
      final loaded = await _catalog!.loadModuleDocs(plan.modules);
      plan.docs = loaded.docs;
      log(plan.describe());
      log('README 合计 ${loaded.context.length} 字符');
    } on Object catch (err) {
      log('规划失败：$err');
    } finally {
      _setBusy(false);
    }
  }

  /// 只生成脚本，不上传。生成完落到编辑区，用户确认后再执行。
  Future<void> generate() async {
    final session = _session;
    if (session == null) return;
    final prompt = promptController.text.trim();
    if (prompt.isEmpty) {
      log('请先输入需求');
      return;
    }
    _setBusy(true, '生成中');
    log('--- 生成 Lua ---');
    try {
      final plan = await session.plan(config, prompt, deviceInfo: deviceInfo);
      lastPlan = plan;
      log(plan.describe());

      final built = await session.buildContext(
        prompt: prompt,
        systemPrompt: config.systemPrompt,
        deviceInfo: deviceInfo,
        history: await _store.loadHistory(),
        plan: plan,
      );
      if (plan.docs.isNotEmpty) log('载入 README：${plan.docs.join(', ')}');
      log(built.describe());

      final longRun = wantsLongRun(prompt, plan.taskKind);
      final budget = longRun && config.maxTokens < longRunMinTokens
          ? longRunMinTokens
          : config.maxTokens;

      final raw = await _llm.call(
        config,
        built.messages,
        maxTokens: budget,
        onProgress: makeProgressReporter(log),
      );
      final script = extractLua(raw);
      scriptController.text = script;
      log('已生成 ${script.length} 字符，可在编辑区修改后上传执行。');
      try {
        validateGeneratedLua(script);
      } on LuaValidationException catch (err) {
        log('注意：本地校验未通过：${err.message}');
      }
    } on Object catch (err) {
      log('生成失败：$err');
    } finally {
      _setBusy(false);
    }
  }

  /// 上传编辑区里的脚本并执行，等待设备返回真实结果。
  Future<void> runCurrentScript() async {
    final bridge = _bridge;
    final session = _session;
    if (bridge == null || session == null) {
      log('请先连接设备');
      return;
    }
    final script = scriptController.text.trim();
    if (script.isEmpty) {
      log('编辑区是空的，没有可执行的脚本');
      return;
    }
    _setBusy(true, '执行中');
    log('--- 上传并执行 ---');
    try {
      validateGeneratedLua(script);
      final outcome = await session.runAndCollect(
        bridge,
        script,
        pollTimeout: Duration(seconds: pollTimeoutS),
        onEvent: log,
      );
      lastJobId = outcome.jobId;
      log(outcome.describe());
      log(outcome.ok ? '执行成功。' : '执行未成功。');
      await _store.appendHistory({
        'prompt': promptController.text.trim(),
        'result': outcome.describe(),
      });
    } on LuaValidationException catch (err) {
      log('本地校验未通过，已拦下未上传：${err.message}');
    } on Object catch (err) {
      log('执行失败：$err');
    } finally {
      _setBusy(false);
    }
  }

  /// 生成并按设备报错自动迭代。
  Future<void> generateAndIterate() async {
    final bridge = _bridge;
    final session = _session;
    if (bridge == null || session == null) {
      log('请先连接设备');
      return;
    }
    final prompt = promptController.text.trim();
    if (prompt.isEmpty) {
      log('请先输入需求');
      return;
    }
    _setBusy(true, '迭代中');
    log('--- 生成并自动迭代（最多 $maxAttempts 轮）---');
    try {
      final result = await session.iterate(
        bridge: bridge,
        config: config,
        prompt: prompt,
        deviceInfo: deviceInfo,
        history: await _store.loadHistory(),
        maxAttempts: maxAttempts,
        pollTimeout: Duration(seconds: pollTimeoutS),
        onEvent: log,
      );
      lastPlan = result.plan;
      final last = result.attempts.isEmpty ? null : result.attempts.last;
      if (last != null && last.script.isNotEmpty) {
        scriptController.text = last.script;
      }
      lastJobId = last?.outcome?.jobId ?? lastJobId;
      log('--- 各轮汇总 ---');
      log(summarizeAttempts(result.attempts));
      if (last?.outcome != null) {
        await _store.appendHistory({
          'prompt': prompt,
          'result': last!.outcome!.describe(),
        });
      }
    } on Object catch (err) {
      log('迭代失败：$err');
    } finally {
      _setBusy(false);
    }
  }

  Future<void> queryJob() async {
    final bridge = _bridge;
    final session = _session;
    if (bridge == null || session == null) {
      log('请先连接设备');
      return;
    }
    try {
      final outcome = await session.pollJobStatus(
        bridge,
        lastJobId,
        // 只看一眼当前状态，不要在这里等到终态。
        pollTimeout: Duration.zero,
        onEvent: log,
      );
      log(outcome.describe());
    } on Object catch (err) {
      log('查询 job 失败：$err');
    }
  }

  Future<void> stopJob() async {
    final bridge = _bridge;
    final session = _session;
    if (bridge == null || session == null) {
      log('请先连接设备');
      return;
    }
    _setBusy(true, '停止中');
    log('正在停止 job…');
    try {
      final result = await session.stopJobAndWait(
        bridge,
        jobId: lastJobId,
        timeout: const Duration(seconds: 5),
        waitAfterStop: const Duration(seconds: 2),
      );
      log('job_stop：${result.stopResponse['data'] ?? result.stopResponse['error'] ?? result.stopResponse}');
      log('最终状态：${result.finalStatus.status}');
      if (result.finalStatus.status != 'unknown') {
        log(result.finalStatus.describe());
      }
    } on Object catch (err) {
      log('停止 job 失败：$err');
    } finally {
      _setBusy(false);
    }
  }

  Future<void> fetchFullLog() async {
    final bridge = _bridge;
    final session = _session;
    if (bridge == null || session == null || lastJobId.isEmpty) {
      log('还没有可查询的 job');
      return;
    }
    final text = await session.fetchJobLog(bridge, lastJobId);
    log(text.isEmpty ? '没取到日志（可能是旧固件不支持 job_tail）' : '完整日志：\n$text');
  }

  void setMaxAttempts(int value) {
    maxAttempts = value.clamp(1, 8);
    notifyListeners();
  }

  void setPollTimeout(int seconds) {
    pollTimeoutS = seconds.clamp(10, 600);
    notifyListeners();
  }

  /// 加载示例脚本到编辑区
  void loadExampleScript(String script) {
    scriptController.text = script;
    log('已加载示例脚本，共 ${script.length} 字符');
    notifyListeners();
  }

  /// 从 assets 加载示例脚本
  Future<void> loadExampleFromAssets(String assetPath, String name) async {
    _setBusy(true, '加载中');
    try {
      final script = await rootBundle.loadString(assetPath);
      scriptController.text = script;
      log('已加载示例「$name」，共 ${script.length} 字符');
    } on Object catch (err) {
      log('加载示例失败：$err');
    } finally {
      _setBusy(false);
    }
  }

  @override
  void dispose() {
    final bridge = _bridge;
    if (bridge != null) unawaited(bridge.disconnect());
    _llm.close();
    scriptController.dispose();
    promptController.dispose();
    super.dispose();
  }
}
