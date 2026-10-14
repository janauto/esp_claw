// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:async';

import 'ble_bridge.dart';
import 'constants.dart';
import 'context_engine.dart';
import 'host_context.dart';
import 'job_outcome.dart';
import 'llm_client.dart';
import 'llm_config.dart';
import 'module_catalog.dart';
import 'playbook_registry.dart';
import 'task_planner.dart';

typedef EventSink = void Function(String message);

/// 生成 -> 上传 -> 执行 -> 读回真实结果 -> 修脚本 的编排层。
///
/// 与界面无关，全部进度通过 [EventSink] 回调抛出，这样单测可以用假 bridge
/// 和固定 LLM 返回覆盖迭代循环的停止条件。
class VibeSession {
  VibeSession({
    required this.catalog,
    required this.registry,
    required this.llm,
  });

  final ModuleCatalog catalog;
  final PlaybookRegistry registry;
  final LlmClient llm;

  /// 取设备信息，作为生成阶段的上下文。
  Future<String> fetchDeviceInfo(Bridge bridge, {Duration? timeout}) async {
    final result = await bridge.request(
      {'id': requestId('info'), 'op': 'device_info'},
      timeout: timeout,
    );
    if (result['ok'] != true) {
      throw BleException('device_info 失败：${result['error'] ?? result}');
    }
    return '${result['data'] ?? ''}';
  }

  Future<Map<String, dynamic>> hello(Bridge bridge, {Duration? timeout}) {
    return bridge.request(
      {'id': requestId('hello'), 'op': 'hello'},
      timeout: timeout,
    );
  }

  /// 规划入口。必须走这里而不是只做关键词匹配：上下文引擎的 plan provider
  /// 只认 TaskPlan，缺了它「实现计划」段会整段静默丢掉。
  Future<TaskPlan> plan(
    LlmConfig config,
    String prompt, {
    String deviceInfo = '',
  }) {
    return planTask(
      prompt,
      catalog,
      (messages, maxTokens) =>
          llm.call(config, messages, maxTokens: maxTokens),
      deviceInfo: deviceInfo,
    );
  }

  Future<BuiltContext> buildContext({
    required String prompt,
    required String systemPrompt,
    String deviceInfo = '',
    List<Map<String, dynamic>> history = const [],
    TaskPlan? plan,
    List<Attempt> attempts = const [],
  }) {
    final builder = buildDefaultBuilder(
      catalog: catalog,
      registry: registry,
      systemPrompt: systemPrompt,
    );
    return builder.build(ContextRequest(
      prompt: prompt,
      deviceInfo: deviceInfo,
      history: history,
      plan: plan,
      attempts: attempts,
    ));
  }

  /// 把脚本写到设备 /scripts/host 下，返回设备上的路径。
  Future<String> uploadLua(Bridge bridge, String script, Duration timeout) async {
    final put = await bridge.request(
      {'id': requestId('lua'), 'op': 'put_lua', 'source': script},
      timeout: timeout,
    );
    if (put['ok'] != true) {
      throw BleException('put_lua 失败：${put['error'] ?? put}');
    }
    final path = '${put['data'] ?? ''}';
    if (path.isEmpty) throw const BleException('put_lua 没有返回脚本路径');
    return path;
  }

  /// 上传、执行，然后轮询 job_status 直到拿到终态或超出耐心。
  ///
  /// 设备端 run_lua 是异步的，只返回 job id；真正的执行结果要靠 job_status 取回。
  ///
  /// [expectsLongRun] 不传时从脚本自身推断，这样手动上传也能正确判定：
  /// 建了 LVGL 屏幕却秒退 = 画面闪一下就没了 = 失败。
  Future<JobOutcome> runAndCollect(
    Bridge bridge,
    String script, {
    Duration timeout = defaultTimeout,
    int? luaTimeoutMs,
    Duration pollTimeout = jobPollTimeout,
    Duration pollInterval = jobPollInterval,
    EventSink? onEvent,
    bool? expectsLongRun,
  }) async {
    final longRun = expectsLongRun ?? scriptExpectsLongRun(script);
    final effectiveTimeoutMs = resolveLuaTimeoutMs(luaTimeoutMs, longRun);
    void report(String message) => onEvent?.call(message);

    final path = await uploadLua(bridge, script, timeout);
    report('已上传到设备：$path');
    if (effectiveTimeoutMs == luaTimeoutUntilCancelled) {
      report('执行窗口：不限时（常驻脚本，跑到被停止为止）');
    } else {
      report('执行窗口：${(effectiveTimeoutMs / 1000).round()}s 后由设备终止');
    }

    final run = await bridge.request(
      {
        'id': requestId('run'),
        'op': 'run_lua',
        'path': path,
        'timeout_ms': effectiveTimeoutMs,
      },
      timeout: timeout,
    );
    final submitText = '${run['data'] ?? run['error'] ?? ''}';
    if (run['ok'] != true) {
      throw BleException('run_lua 失败：${submitText.isEmpty ? run : submitText}');
    }

    final jobId = extractJobId(submitText);
    report('已提交执行：job_id=${jobId.isEmpty ? '(未解析到)' : jobId}');

    final outcome = await pollJobStatus(
      bridge,
      jobId,
      timeout: timeout,
      pollTimeout: pollTimeout,
      pollInterval: pollInterval,
      onEvent: onEvent,
    );

    var log = outcome.log;
    if (outcome.logTruncated && !outcome.copyWith(expectsLongRun: longRun).ok) {
      // job_status 只带一段有限的日志尾巴，报错的 traceback 可能被截断。
      final full = await fetchJobLog(
        bridge,
        outcome.jobId.isEmpty ? jobId : outcome.jobId,
        timeout: timeout,
      );
      if (full.isNotEmpty) {
        report('已拉取完整日志（job_status 的尾巴被截断）');
        log = full;
      }
    }

    return outcome.copyWith(
      jobId: outcome.jobId.isEmpty ? jobId : outcome.jobId,
      log: log,
      submitText: submitText,
      expectsLongRun: longRun,
    );
  }

  /// 用 job_tail 从头拉日志。不支持该 op 的旧固件会返回错误，此时返回空串。
  Future<String> fetchJobLog(
    Bridge bridge,
    String jobId, {
    Duration timeout = defaultTimeout,
    int maxBytes = 3000,
  }) async {
    if (jobId.isEmpty) return '';
    Map<String, dynamic> response;
    try {
      response = await bridge.request(
        {
          'id': requestId('tail'),
          'op': 'job_tail',
          'job_id': jobId,
          'since_seq': 0,
          'max_bytes': maxBytes,
        },
        timeout: timeout,
      );
    } on Object {
      return '';
    }
    if (response['ok'] != true) return '';
    final text = '${response['data'] ?? ''}';
    const marker = '\nlog=';
    final position = text.indexOf(marker);
    return position >= 0 ? text.substring(position + marker.length) : '';
  }

  /// 轮询到终态为止；到点还在 running 就按 running 返回（长跑脚本是正常的）。
  Future<JobOutcome> pollJobStatus(
    Bridge bridge,
    String jobId, {
    Duration timeout = defaultTimeout,
    Duration pollTimeout = jobPollTimeout,
    Duration pollInterval = jobPollInterval,
    EventSink? onEvent,
  }) async {
    final started = DateTime.now();
    final deadline = started.add(pollTimeout);
    var last = JobOutcome(jobId: jobId, status: 'unknown');
    var lastStatus = '';
    var lastBeat = started;

    while (true) {
      final command = <String, dynamic>{
        'id': requestId('job'),
        'op': 'job_status',
      };
      if (jobId.isNotEmpty) command['job_id'] = jobId;
      final status = await bridge.request(command, timeout: timeout);
      final text = '${status['data'] ?? status['error'] ?? ''}';

      if (status['ok'] == true) {
        last = parseJobStatus(text);
        if (last.status != lastStatus) {
          onEvent?.call('job 状态：${last.status}');
          lastBeat = DateTime.now();
        }
        lastStatus = last.status;
        if (terminalJobStatus.contains(last.status)) return last;
        // 长跑脚本会一直停在 running：状态不变也要间歇报一声，
        // 否则等满 pollTimeout 的这段时间界面上什么都没有。
        final now = DateTime.now();
        if (onEvent != null && now.difference(lastBeat) >= jobHeartbeat) {
          onEvent('脚本仍在设备上运行（已 ${now.difference(started).inSeconds}s）');
          lastBeat = now;
        }
      } else {
        onEvent?.call('job_status 查询失败：$text');
      }

      if (!DateTime.now().isBefore(deadline)) return last;
      await Future<void>.delayed(pollInterval);
    }
  }

  Future<Map<String, dynamic>> stopJob(
    Bridge bridge,
    String jobId, {
    Duration timeout = defaultTimeout,
  }) {
    final command = <String, dynamic>{'id': requestId('stop'), 'op': 'job_stop'};
    if (jobId.isNotEmpty) command['job_id'] = jobId;
    return bridge.request(command, timeout: timeout);
  }

  /// 快速停止 job：发送停止请求并短暂等待确认，不做长时间轮询。
  ///
  /// 返回 (stopResponse, finalStatus)。
  Future<({Map<String, dynamic> stopResponse, JobOutcome finalStatus})> stopJobAndWait(
    Bridge bridge, {
    String jobId = '',
    Duration timeout = defaultTimeout,
    Duration waitAfterStop = const Duration(seconds: 2),
  }) async {
    // 发送停止请求
    final stopResp = await stopJob(bridge, jobId, timeout: timeout);

    // 等待一小段时间让设备处理
    await Future<void>.delayed(const Duration(milliseconds: 500));

    // 轮询几次确认状态（最多 waitAfterStop 秒）
    final started = DateTime.now();
    var lastOutcome = JobOutcome(jobId: jobId, status: 'unknown');

    while (DateTime.now().difference(started) < waitAfterStop) {
      final command = <String, dynamic>{
        'id': requestId('job'),
        'op': 'job_status',
      };
      if (jobId.isNotEmpty) command['job_id'] = jobId;

      final statusResp = await bridge.request(command, timeout: timeout);
      final text = '${statusResp['data'] ?? statusResp['error'] ?? ''}';
      if (statusResp['ok'] == true) {
        lastOutcome = parseJobStatus(text);
        // 如果已经到达终态，立即返回
        if (terminalJobStatus.contains(lastOutcome.status)) break;
      }
      await Future<void>.delayed(const Duration(milliseconds: 300));
    }

    return (stopResponse: stopResp, finalStatus: lastOutcome);
  }


  /// 生成 → 执行 → 读回真实结果 → 修脚本，直到成功或判定继续重试没意义。
  ///
  /// 刻意不是"失败就重来 N 次"：每一轮都要先确认再花一次 LLM 调用是有依据的，
  /// 否则重复烧 token 只会产出几乎一样的脚本。
  Future<({List<Attempt> attempts, TaskPlan plan})> iterate({
    required Bridge bridge,
    required LlmConfig config,
    required String prompt,
    String deviceInfo = '',
    List<Map<String, dynamic>> history = const [],
    TaskPlan? plan,
    int maxAttempts = 1,
    Duration timeout = defaultTimeout,
    int? luaTimeoutMs,
    Duration pollTimeout = jobPollTimeout,
    EventSink? onEvent,
  }) async {
    void report(String message) => onEvent?.call(message);

    var resolvedPlan = plan;
    if (resolvedPlan == null) {
      report('正在规划实现方案…');
      resolvedPlan = await this.plan(config, prompt, deviceInfo: deviceInfo);
      report(resolvedPlan.describe());
    }

    final attempts = <Attempt>[];
    final seenScripts = <String>{};
    var lastSignature = '';

    // 游戏/界面类任务的成败方向和一次性脚本相反：done 且秒退是失败，timeout 才是成功。
    final longRun = wantsLongRun(prompt, resolvedPlan.taskKind);
    if (longRun) {
      report('判定为常驻类任务：done 且运行 <${longRunMinS}s 视为失败（画面闪一下就退）');
    }

    // 完整游戏脚本 200+ 行，小预算装不下；截断后上传的是半个程序，
    // 语法能过但秒退。常驻类任务直接把预算提到够写完一份完整脚本。
    final genBudget = longRun && config.maxTokens < longRunMinTokens
        ? longRunMinTokens
        : config.maxTokens;
    if (genBudget > config.maxTokens) {
      report('常驻类任务生成预算提到 max_tokens=$genBudget'
          '（配置值 ${config.maxTokens} 装不下完整脚本）');
    }

    final rounds = maxAttempts < 1 ? 1 : maxAttempts;
    for (var index = 1; index <= rounds; index++) {
      final built = await buildContext(
        prompt: prompt,
        systemPrompt: config.systemPrompt,
        deviceInfo: deviceInfo,
        history: history,
        plan: resolvedPlan,
        attempts: attempts,
      );
      if (index == 1) {
        if (resolvedPlan.docs.isNotEmpty) {
          report('载入 README：${resolvedPlan.docs.join(', ')}');
        }
        report(built.describe());
      }
      report('第 $index/$rounds 轮：正在生成 Lua…');

      final String raw;
      try {
        raw = await llm.call(
          config,
          built.messages,
          maxTokens: genBudget,
          onProgress: makeProgressReporter(report),
        );
      } on Object catch (err) {
        final attempt = Attempt(index: index, script: '', error: 'LLM 调用失败：$err');
        attempts.add(attempt);
        report(attempt.error);
        break;
      }

      final script = extractLua(raw);
      final attempt = Attempt(index: index, script: script, raw: raw);

      final fingerprint = normalizeScript(script);
      if (fingerprint.isNotEmpty && seenScripts.contains(fingerprint)) {
        attempt.stoppedBecause = 'LLM 重新生成了与上一轮实质相同的脚本，继续重试不会有新结果';
        attempts.add(attempt);
        report(attempt.stoppedBecause);
        break;
      }
      seenScripts.add(fingerprint);

      try {
        validateGeneratedLua(script);
      } on LuaValidationException catch (err) {
        attempt.error = err.message;
        attempts.add(attempt);
        report('本地校验未通过：${err.message}');
        if (index >= rounds) break;
        continue;
      }

      report('正在上传并执行…');
      try {
        attempt.outcome = await runAndCollect(
          bridge,
          script,
          timeout: timeout,
          luaTimeoutMs: luaTimeoutMs,
          pollTimeout: pollTimeout,
          onEvent: onEvent,
          expectsLongRun: longRun,
        );
      } on Object catch (err) {
        attempt.error = '设备执行链路出错：$err';
        attempts.add(attempt);
        report(attempt.error);
        break;
      }

      attempts.add(attempt);
      final outcome = attempt.outcome!;
      report(outcome.describe());

      if (outcome.ok) {
        if (outcome.expectsLongRun &&
            (outcome.status == 'running' || outcome.status == 'queued')) {
          report('执行成功：已稳定运行 ${outcome.runtimeS}s，脚本仍在设备上跑着。');
        } else {
          report('执行成功。');
        }
        break;
      }
      if (!outcome.retryable) {
        if (outcome.status == 'unknown') {
          attempt.stoppedBecause = '没能读到 job 状态，无法判断脚本对错，不再重试';
        } else if (outcome.status == 'running' || outcome.status == 'queued') {
          attempt.stoppedBecause =
              '等待 ${pollTimeout.inSeconds}s 后 job 仍在 ${outcome.status}，'
              '脚本还在跑（长跑脚本属正常），不按失败重试';
        } else {
          attempt.stoppedBecause =
              'status=${outcome.status} 不属于需要改脚本的失败（长跑脚本或被主动停止），不再重试';
        }
        report(attempt.stoppedBecause);
        break;
      }
      if (!outcome.hasDiagnostics) {
        attempt.stoppedBecause = '设备没有返回 summary 或日志，缺少可用于修脚本的信息，不再重试';
        report(attempt.stoppedBecause);
        break;
      }

      final signature = outcome.errorSignature();
      if (signature == lastSignature) {
        attempt.stoppedBecause = '连续两轮同一个错误（$signature），LLM 没有收敛，不再重试';
        report(attempt.stoppedBecause);
        break;
      }
      lastSignature = signature;
    }

    return (attempts: attempts, plan: resolvedPlan);
  }
}

/// 把每轮的状态和停止原因汇总成一段可读文本，方便判断 token 花在哪。
String summarizeAttempts(List<Attempt> attempts) {
  if (attempts.isEmpty) return '没有产生任何尝试。';
  final lines = <String>[];
  for (final attempt in attempts) {
    final outcome = attempt.outcome;
    final status = outcome != null
        ? 'status=${outcome.status}'
            '${outcome.runtimeS != 0 ? ' runtime=${outcome.runtimeS}s' : ''}'
        : (attempt.error.isNotEmpty ? '未执行' : '无结果');
    lines.add('第 ${attempt.index} 轮：$status'
        '${attempt.ok ? ' 成功' : ''}');
    if (attempt.error.isNotEmpty) lines.add('  ${attempt.error}');
    if (attempt.stoppedBecause.isNotEmpty) lines.add('  停止：${attempt.stoppedBecause}');
  }
  return lines.join('\n');
}
