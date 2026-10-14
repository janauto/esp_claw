// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:async';
import 'dart:convert';

import 'package:http/http.dart' as http;

import 'constants.dart';
import 'llm_config.dart';

/// 服务不接受 stream 参数，可以退回非流式重试。
///
/// 只有这一种情况值得重试，所以单独立一个类型：靠匹配报错字符串来判断会把
/// "输出被截断"之类的正常失败也误判成可重试，白烧一次请求还把真正的原因盖掉。
class StreamUnsupported implements Exception {
  const StreamUnsupported(this.message);

  final String message;

  @override
  String toString() => message;
}

class LlmException implements Exception {
  const LlmException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// 流式进度回调。[isReasoning] 区分思维链和正文。
typedef ProgressCallback = void Function(String delta, bool isReasoning);

/// 从流式吐出来的 Lua 里认出"模型正在写什么"。按行匹配，先到先报。
final List<({RegExp re, String template})> streamLandmarks = [
  (re: RegExp(r'''require\s*\(?\s*['"]([\w.]+)['"]'''), template: '引入 {0} 模块'),
  (re: RegExp(r'^\s*local\s+function\s+([\w.:]+)', multiLine: true), template: '写 {0}() 函数'),
  (re: RegExp(r'^\s*function\s+([\w.:]+)', multiLine: true), template: '写 {0}() 函数'),
  (re: RegExp(r'lvgl\.create_screen|lvgl\.init'), template: '创建 LVGL 屏幕'),
  (re: RegExp(r'lvgl\.(?:create_)?label'), template: '添加文字标签'),
  (re: RegExp(r'lvgl\.(?:create_)?(?:obj|object|button|btn)'), template: '添加界面对象'),
  (re: RegExp(r'indev_register'), template: '注册触摸输入'),
  (re: RegExp(r'lvgl\.process_events|lvgl\.run\s*\('), template: '写主循环'),
  (re: RegExp(r'display\.init'), template: '初始化屏幕'),
];

/// 把流式增量翻译成人话进度。生成一份完整脚本要几十秒，界面上不能一片空白。
///
/// 返回的回调可以直接交给 [callOpenAiCompatible] 的 onProgress。
ProgressCallback makeProgressReporter(void Function(String message) report) {
  final seen = <String>{};
  var buffer = '';
  var reasoningChars = 0;
  var phase = '';
  var lastBeat = DateTime.now();
  final started = DateTime.now();

  void beat(String suffix) {
    final elapsed = DateTime.now().difference(started).inSeconds;
    report('$suffix（已 ${elapsed}s）');
    lastBeat = DateTime.now();
  }

  return (String delta, bool isReasoning) {
    if (isReasoning) {
      reasoningChars += delta.length;
      if (phase.isEmpty || phase == 'waiting') {
        phase = 'reasoning';
        beat('模型正在推理方案');
      } else if (phase == 'reasoning' &&
          DateTime.now().difference(lastBeat) >= streamHeartbeat) {
        beat('仍在推理（已产出 $reasoningChars 字思维链）');
      }
      return;
    }

    if (delta.isEmpty) {
      // 保活帧：只说明连接还活着。
      if (phase.isEmpty) {
        phase = 'waiting';
        beat('已连上模型，等待首个字符');
      } else if (DateTime.now().difference(lastBeat) >= streamHeartbeat) {
        beat(phase == 'reasoning' ? '仍在推理' : '仍在生成');
      }
      return;
    }

    if (phase != 'content') {
      phase = 'content';
      beat('模型开始写脚本');
    }

    buffer += delta;
    for (final landmark in streamLandmarks) {
      final match = landmark.re.firstMatch(buffer);
      if (match == null) continue;
      final captured = match.groupCount >= 1 ? (match.group(1) ?? '') : '';
      final message = landmark.template.replaceFirst('{0}', captured);
      if (seen.add(message)) {
        report('  $message');
        lastBeat = DateTime.now();
      }
    }
    // 只保留尾部，避免长脚本让正则每次都扫全文。
    if (buffer.length > 4000) {
      buffer = buffer.substring(buffer.length - 2000);
    }
    if (DateTime.now().difference(lastBeat) >= streamHeartbeat) {
      beat('仍在生成（已写 ${buffer.length} 字符）');
    }
  };
}

/// OpenAI 兼容接口客户端。
class LlmClient {
  LlmClient({http.Client? client}) : _client = client ?? http.Client();

  final http.Client _client;

  void close() => _client.close();

  http.Request _buildRequest(
    LlmConfig config,
    List<Map<String, String>> messages,
    int budget, {
    required bool stream,
  }) {
    final uri = Uri.parse('${_stripSlash(config.baseUrl)}/chat/completions');
    final request = http.Request('POST', uri);
    request.headers['Content-Type'] = 'application/json';
    if (config.authType == 'bearer') {
      request.headers['Authorization'] = 'Bearer ${config.apiKey}';
    }

    final payload = <String, dynamic>{
      'model': config.model,
      'max_tokens': budget,
      'messages': messages,
    };
    if (stream) {
      payload['stream'] = true;
      // 有 usage 才能在截断时报出真实的 completion_tokens。
      payload['stream_options'] = {'include_usage': true};
      request.headers['Accept'] = 'text/event-stream';
    }
    request.bodyBytes = utf8.encode(jsonEncode(payload));
    return request;
  }

  LlmException _timeoutError(Duration timeout, int budget, {required bool stream}) {
    if (stream) {
      return LlmException('LLM 连接已卡住：${timeout.inSeconds}s 没有收到任何新内容'
          '（max_tokens=$budget）。可能是网络中断或服务无响应。');
    }
    return LlmException('LLM 请求超时：${timeout.inSeconds}s 内没读到完整回复'
        '（max_tokens=$budget）。可以调小 max_tokens，或换更快的服务。');
  }

  /// 流式读取回复，边收边把进度回报出去。
  ///
  /// 非流式请求在模型写完之前没有任何输出，界面上就是"卡住不动"。
  /// 流式还有一个好处：超时语义从"总耗时"变成"多久没有新内容"，
  /// 写得慢（正常）和真的断了（异常）从此可以区分。
  Future<String> _stream(
    LlmConfig config,
    List<Map<String, String>> messages,
    int budget,
    ProgressCallback onProgress,
  ) async {
    final request = _buildRequest(config, messages, budget, stream: true);
    final content = StringBuffer();
    final reasoning = StringBuffer();
    var finishReason = '';
    Object? completionTokens;

    http.StreamedResponse response;
    try {
      response = await _client.send(request).timeout(httpStreamStallTimeout);
    } on TimeoutException {
      throw _timeoutError(httpStreamStallTimeout, budget, stream: true);
    } on Object catch (err) {
      throw LlmException('LLM 连接失败：$err（base_url=${config.baseUrl}）');
    }

    if (response.statusCode >= 400) {
      final detail = await response.stream.bytesToString();
      if (streamUnsupportedHttpCodes.contains(response.statusCode)) {
        throw StreamUnsupported('LLM 请求失败：HTTP ${response.statusCode} $detail');
      }
      throw LlmException('LLM 请求失败：HTTP ${response.statusCode} $detail');
    }

    onProgress('', false);
    final lines = response.stream
        .transform(utf8.decoder)
        .transform(const LineSplitter());

    try {
      // 每一行都重置静默计时：这里的超时是"多久没有新内容"，不是总耗时。
      await for (final raw in lines.timeout(httpStreamStallTimeout)) {
        final line = raw.trim();
        if (line.isEmpty || !line.startsWith('data:')) {
          // 空行和 ": ping" 之类的保活帧没有内容，但足以说明连接是活的。
          onProgress('', false);
          continue;
        }
        final data = line.substring('data:'.length).trim();
        if (data == '[DONE]') break;

        Map<String, dynamic> chunk;
        try {
          final decoded = jsonDecode(data);
          if (decoded is! Map<String, dynamic>) continue;
          chunk = decoded;
        } on FormatException {
          continue;
        }

        final usage = chunk['usage'];
        if (usage is Map && usage['completion_tokens'] != null) {
          completionTokens = usage['completion_tokens'];
        }

        final choices = chunk['choices'];
        if (choices is! List || choices.isEmpty) continue;
        final choice = choices.first;
        if (choice is! Map) continue;
        final reason = choice['finish_reason'];
        if (reason is String && reason.isNotEmpty) finishReason = reason;

        final delta = choice['delta'];
        if (delta is! Map) continue;

        final piece = delta['content'];
        if (piece is String && piece.isNotEmpty) {
          content.write(piece);
          onProgress(piece, false);
        }

        final think = delta['reasoning_content'] ?? delta['reasoning'];
        if (think is String && think.isNotEmpty) {
          reasoning.write(think);
          onProgress(think, true);
        }
      }
    } on TimeoutException {
      throw _timeoutError(httpStreamStallTimeout, budget, stream: true);
    }

    final text = content.toString();
    if (text.trim().isNotEmpty) {
      if (finishReason == 'length') {
        throw LlmException('LLM 输出被 max_tokens 截断，脚本不完整，已丢弃。'
            '（completion_tokens=${completionTokens ?? '?'}，本次 max_tokens=$budget）'
            '请在设置里调大 max_tokens。');
      }
      return text;
    }

    // thinking 模型可能把正文写进 reasoning_content，content 全空。
    final thinkText = reasoning.toString();
    if (thinkText.contains('```')) return thinkText;
    if (finishReason == 'length') {
      throw LlmException('LLM 只产出了思维链就被 max_tokens 截断，没有正文。'
          '（completion_tokens=${completionTokens ?? '?'}，本次 max_tokens=$budget）'
          '请调大 max_tokens。');
    }
    throw LlmException(
        'LLM 返回为空（finish_reason=${finishReason.isEmpty ? '?' : finishReason}）');
  }

  Future<String> call(
    LlmConfig config,
    List<Map<String, String>> messages, {
    int? maxTokens,
    ProgressCallback? onProgress,
  }) async {
    if (config.baseUrl.isEmpty || config.model.isEmpty) {
      throw const LlmException('请先配置 base_url 和 model');
    }
    if (config.authType != 'none' && config.apiKey.isEmpty) {
      throw const LlmException('请先配置 API Key，或把鉴权方式设为 none');
    }

    final budget = maxTokens ?? config.maxTokens;

    if (onProgress != null) {
      try {
        return await _stream(config, messages, budget, onProgress);
      } on StreamUnsupported {
        // 少数服务不认 stream 参数，退回非流式重试一次，别让整轮白跑。
        // 其余失败（截断、鉴权、超时）直接抛：重试治不好，还会盖掉真正的原因。
      }
    }

    final request = _buildRequest(config, messages, budget, stream: false);
    final timeout = httpTimeoutFor(budget);
    Map<String, dynamic> payload;
    try {
      final streamed = await _client.send(request).timeout(timeout);
      final body = await streamed.stream.bytesToString().timeout(timeout);
      if (streamed.statusCode >= 400) {
        throw LlmException('LLM 请求失败：HTTP ${streamed.statusCode} $body');
      }
      final decoded = jsonDecode(body);
      if (decoded is! Map<String, dynamic>) {
        throw const LlmException('LLM 返回的不是 JSON 对象');
      }
      payload = decoded;
    } on TimeoutException {
      throw _timeoutError(timeout, budget, stream: false);
    } on LlmException {
      rethrow;
    } on Object catch (err) {
      throw LlmException('LLM 连接失败：$err（base_url=${config.baseUrl}）');
    }

    final choices = payload['choices'];
    final choice = (choices is List && choices.isNotEmpty) ? choices.first : const {};
    final message = (choice is Map ? choice['message'] : null) ?? const {};
    final content = message is Map ? message['content'] : null;
    final usage = payload['usage'];
    final finish =
        (choice is Map ? choice['finish_reason'] as String? : null) ?? '?';

    if (content is String && content.trim().isNotEmpty) {
      if (finish == 'length') {
        // 截断的回复常常缺收尾 fence，或只剩程序前半段：语法上能通过、上传后
        // 1 秒就退出（屏幕闪一下回主界面）。必须当场判失败。
        final tokens = usage is Map ? usage['completion_tokens'] ?? '?' : '?';
        throw LlmException('LLM 输出被 max_tokens 截断，脚本不完整，已丢弃。'
            '（completion_tokens=$tokens，本次 max_tokens=$budget）'
            '请在设置里调大 max_tokens。');
      }
      return content;
    }

    // thinking 类模型会把内容放在 reasoning_content，content 为空。
    final reasoning =
        message is Map ? (message['reasoning_content'] ?? message['reasoning']) : null;
    if (reasoning is String && reasoning.contains('```')) return reasoning;

    var detail = 'finish_reason=$finish';
    if (usage is Map) {
      detail += ', prompt_tokens=${usage['prompt_tokens'] ?? '?'}'
          ', completion_tokens=${usage['completion_tokens'] ?? '?'}';
    }
    if (finish == 'length') {
      detail += '。输出被 max_tokens 截断，请调大 max_tokens';
    } else if (reasoning is String && reasoning.trim().isNotEmpty) {
      detail += '。模型只产出了思维链没有正文，请调大 max_tokens';
    }
    throw LlmException('LLM 返回为空（$detail）');
  }

  static String _stripSlash(String value) =>
      value.endsWith('/') ? value.substring(0, value.length - 1) : value;
}
