// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'constants.dart';

/// 一次设备端执行的结果，从 job_status 的 key=value 文本解析而来。
class JobOutcome {
  const JobOutcome({
    required this.jobId,
    required this.status,
    this.summary = '',
    this.log = '',
    this.runtimeS = 0,
    this.submitText = '',
    this.logTruncated = false,
    this.expectsLongRun = false,
  });

  final String jobId;
  final String status;
  final String summary;
  final String log;
  final int runtimeS;
  final String submitText;
  final bool logTruncated;

  /// 本次任务是不是本该长跑（游戏、交互界面）。判定成败的方向完全相反，见 [ok]。
  final bool expectsLongRun;

  JobOutcome copyWith({
    String? jobId,
    String? status,
    String? summary,
    String? log,
    int? runtimeS,
    String? submitText,
    bool? logTruncated,
    bool? expectsLongRun,
  }) {
    return JobOutcome(
      jobId: jobId ?? this.jobId,
      status: status ?? this.status,
      summary: summary ?? this.summary,
      log: log ?? this.log,
      runtimeS: runtimeS ?? this.runtimeS,
      submitText: submitText ?? this.submitText,
      logTruncated: logTruncated ?? this.logTruncated,
      expectsLongRun: expectsLongRun ?? this.expectsLongRun,
    );
  }

  bool get ok {
    if (!expectsLongRun) return status == 'done';
    // 界面/游戏脚本必须一直跑，成败方向和一次性脚本相反：
    // - running：不限时执行下的正常终局。轮询到点脚本还在跑，画面就还在，
    //   这是游戏类任务最主要的成功信号（设备不会再报 timeout）。
    // - timeout：旧固件/显式限时下的成功信号，主循环转到了被掐掉那一刻。
    // - done 且秒退：画面闪一下就回主界面，失败。
    if (status == 'running' || status == 'queued') return runtimeS >= longRunMinS;
    if (status == 'timeout') return true;
    if (status == 'done') return runtimeS >= longRunMinS;
    return false;
  }

  /// 界面脚本秒退：屏幕闪一下就回到主界面。
  bool get flickered =>
      expectsLongRun && status == 'done' && runtimeS < longRunMinS;

  /// 设备是否真的给出了可用于修脚本的信息。
  bool get hasDiagnostics {
    // 设备端不会为"跑完就退"报错，诊断由上位机合成，见 flickerDiagnosis。
    if (flickered) return true;
    return meaningful(summary).isNotEmpty || meaningful(log).isNotEmpty;
  }

  static String meaningful(String text) {
    final stripped = text.trim();
    const noise = {'', '(empty)', '(none)', '(unnamed)'};
    return noise.contains(stripped) ? '' : stripped;
  }

  /// 只有真正的 Lua 报错值得再花一次 LLM 调用。
  ///
  /// timeout 通常意味着脚本是长跑的（游戏主循环、交互界面），本身就是预期行为；
  /// stopped 是有人主动停的。这两种重试只会白烧 token。
  bool get retryable {
    if (status == 'failed') return true;
    // 缺主循环是模型自己能改的，且我们能给出明确诊断，值得再要一轮。
    if (flickered) return true;
    if (status == 'timeout') {
      if (expectsLongRun) return false;
      // 跑起来又有日志输出，说明脚本是在正常长跑，不是写错了。
      return !hasDiagnostics && runtimeS <= 1;
    }
    return false;
  }

  /// 用于判断两轮是不是同一个错误：取日志里第一条 Lua 报错。
  String errorSignature() {
    final text = meaningful(log).isNotEmpty ? meaningful(log) : meaningful(summary);
    if (text.isEmpty) return '$status:no-diagnostics';
    final match = RegExp(r'[^\n]*(?:error|Error|ERROR|attempt to|bad argument)[^\n]*')
        .firstMatch(text);
    final line = match?.group(0) ??
        text.trim().split('\n').where((l) => l.isNotEmpty).lastOrNull ??
        text.trim();
    // 去掉脚本路径里的 job id，避免同一个错误因为路径不同被当成两个。
    final normalized = line.trim().replaceAll(RegExp(r'[0-9a-f]{6,}'), 'ID');
    final clipped =
        normalized.length > 180 ? normalized.substring(0, 180) : normalized;
    return '$status:$clipped';
  }

  String describe() {
    final parts = <String>['status=$status', 'job_id=${jobId.isEmpty ? '(未知)' : jobId}'];
    if (runtimeS != 0) parts.add('runtime_s=$runtimeS');
    var head = parts.join(' ');
    if (flickered) {
      head += '\n${flickerDiagnosis.replaceFirst('{runtime}', '$runtimeS')}';
    }
    final sum = meaningful(summary);
    final logText = meaningful(log);
    if (sum.isNotEmpty) head += '\nsummary: $sum';
    if (logText.isNotEmpty) {
      final tail = logText.length > 1800
          ? logText.substring(logText.length - 1800)
          : logText;
      head += '\n设备日志：\n$tail';
    }
    if (sum.isEmpty && logText.isEmpty) {
      head += '\n设备没有返回 summary 或日志。';
    }
    return head;
  }
}

/// 一轮生成 + 执行的完整记录。
class Attempt {
  Attempt({
    required this.index,
    required this.script,
    this.raw = '',
    this.outcome,
    this.error = '',
    this.stoppedBecause = '',
  });

  final int index;
  final String script;
  final String raw;
  JobOutcome? outcome;
  String error;
  String stoppedBecause;

  bool get ok => outcome?.ok ?? false;
}

/// 解析 cap_lua 的 job_status 纯文本输出。
///
/// 格式是 key=value 逐行，recent_log 在最后且值里可能带换行，所以要单独切出来。
JobOutcome parseJobStatus(String text) {
  var log = '';
  var head = text;
  for (final marker in ['\nrecent_log=', '\nlog=']) {
    final position = head.indexOf(marker);
    if (position >= 0) {
      log = head.substring(position + marker.length);
      head = head.substring(0, position);
      break;
    }
  }

  final fields = <String, String>{};
  for (final line in head.split('\n')) {
    final index = line.indexOf('=');
    if (index >= 0) {
      fields[line.substring(0, index).trim()] = line.substring(index + 1).trim();
    }
  }

  return JobOutcome(
    jobId: fields['job_id'] ?? '',
    status: fields['status'] ?? 'unknown',
    summary: fields['summary'] ?? '',
    log: log,
    runtimeS: int.tryParse(fields['runtime_s'] ?? '') ?? 0,
    logTruncated: (fields['log_truncated'] ?? '').toLowerCase() == 'true',
  );
}

/// 从 run_lua 的返回文本里取 job id（设备端返回的是人类可读文本，不是 JSON）。
String extractJobId(String text) {
  for (final pattern in [r'job_id=([0-9a-zA-Z]+)', r'Lua job ([0-9a-zA-Z]+)']) {
    final match = RegExp(pattern).firstMatch(text);
    if (match != null) return match.group(1)!;
  }
  return '';
}

/// 用于比较两轮脚本是否实质相同：忽略空白与注释差异。
String normalizeScript(String script) {
  final lines = <String>[];
  for (var line in script.split('\n')) {
    line = line.replaceAll(RegExp(r'--.*$'), '').trim();
    if (line.isNotEmpty) lines.add(line.replaceAll(RegExp(r'\s+'), ' '));
  }
  return lines.join('\n');
}

/// 常驻脚本的两种合法形态，见 lua_module_lvgl/README.md。
final RegExp lvglResidentRe =
    RegExp(r'lvgl\.run\s*\(|while\s+.*\s+do[\s\S]*?lvgl\.process_events');

/// 建了屏幕/控件就说明这脚本要出画面，画面必须撑住才有意义。
final RegExp _uiScriptRe =
    RegExp(r'lvgl\.(?:create_screen|init)\s*\(|display\.init\s*\(');

/// 从脚本本身判断它该不该常驻。
///
/// prompt 不总是可靠（手动上传时根本没有 prompt），但两件事是确定的：
/// - 建了 LVGL 屏幕：Lua 一退出画面就销毁，done+秒退必然是失败；
/// - 写了驻留主循环：脚本自己声明了要一直跑，不该给有限的执行窗口。
/// 只认第一条会漏掉"复用已初始化的屏幕、只写主循环"的脚本。
bool scriptExpectsLongRun(String script) =>
    _uiScriptRe.hasMatch(script) || lvglResidentRe.hasMatch(script);

/// 本地校验不通过时抛出，避免把半个程序上传到设备白跑一次往返。
class LuaValidationException implements Exception {
  const LuaValidationException(this.message);

  final String message;

  @override
  String toString() => message;
}

void validateGeneratedLua(String script) {
  final errors = <String>[];
  final body = script.trim();

  // "上传了不完整的东西"在这里拦掉，省一次设备往返。
  if (body.contains('```') || RegExp(r'^\s*`', multiLine: true).hasMatch(body)) {
    errors.add('脚本里残留 Markdown 代码围栏或反引号，说明模型输出被截断，需重新生成完整脚本');
  }
  if (!body.contains('require(') && !body.contains('require ')) {
    errors.add('脚本没有任何 require()，这是代码片段而不是完整脚本，必须输出可独立运行的整份脚本');
  }

  // 秒退：只建对象不驻留，Lua 一退出 LVGL 对象全销毁，屏幕闪一下就回主界面。
  // 判据取自脚本本身而不是 taskKind：手动上传时没有 plan，但"建了屏幕"是确定的。
  if (scriptExpectsLongRun(body) &&
      body.contains('lvgl') &&
      !lvglResidentRe.hasMatch(body)) {
    errors.add(
      '脚本创建了 LVGL 界面但没有常驻主循环，执行后会立刻退出、画面闪一下就回主界面。'
      '末尾必须有 `while true do ... lvgl.process_events(16) end` 或 `lvgl.run(...)`',
    );
  }

  if (errors.isNotEmpty) {
    throw LuaValidationException(errors.join('；'));
  }
}

final RegExp _luaFenceRe =
    RegExp(r'```[ \t]*(?:lua)?[ \t]*\r?\n([\s\S]*?)```', caseSensitive: false);
final RegExp _luaOpenFenceRe =
    RegExp(r'```[ \t]*(?:lua)?[ \t]*\r?\n([\s\S]*)$', caseSensitive: false);

/// 越像"完整程序"排越前：有 require / 主循环的加权，其次比长度。
int _luaBlockRank(String block) {
  var weight = 0;
  if (RegExp(r'\brequire\s*\(').hasMatch(block)) weight += 2;
  if (RegExp(r'^\s*(?:while|for)\b', multiLine: true).hasMatch(block)) weight += 1;
  return weight;
}

/// 从 LLM 回复里取出 Lua 源码。
///
/// 刻意不取"第一个"代码块：模型常先给一小段示意片段、再给完整实现，
/// 取第一个会把片段当成整个程序上传。也要处理被 max_tokens 截断、
/// 收尾 fence 缺失的回复，否则整段原文连 ``` 一起被当成 Lua 上传。
String extractLua(String text) {
  var blocks = _luaFenceRe
      .allMatches(text)
      .map((m) => m.group(1)!)
      .toList(growable: false);
  if (blocks.isEmpty) {
    // 没有成对 fence，说明回复被截断：取开启 fence 之后的全部内容。
    final open = _luaOpenFenceRe.firstMatch(text);
    blocks = [open != null ? open.group(1)! : text];
  }

  var best = blocks.first;
  var bestRank = _luaBlockRank(best);
  for (final block in blocks.skip(1)) {
    final rank = _luaBlockRank(block);
    if (rank > bestRank || (rank == bestRank && block.length > best.length)) {
      best = block;
      bestRank = rank;
    }
  }

  // 兜底：无论走哪条路径，都不允许 fence 残留流到设备。
  final kept = best
      .split('\n')
      .where((line) => !line.trimLeft().startsWith('```'))
      .join('\n');
  return kept.trim();
}
