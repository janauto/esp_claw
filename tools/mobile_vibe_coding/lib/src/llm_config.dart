// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';

import 'constants.dart';

/// 手机端 LLM 配置。与电脑端 HostLlmConfig 字段一致，便于两边互相拷配置。
class LlmConfig {
  const LlmConfig({
    this.provider = 'deepseek',
    this.baseUrl = 'https://api.deepseek.com/v1',
    this.model = 'deepseek-v4-pro',
    this.apiKey = '',
    this.authType = 'bearer',
    this.maxTokens = defaultMaxTokens,
    this.systemPrompt = '你是 ESP-Claw 手机端上位机的 Lua 生成器。',
  });

  final String provider;
  final String baseUrl;
  final String model;
  final String apiKey;
  final String authType;
  final int maxTokens;
  final String systemPrompt;

  LlmConfig copyWith({
    String? provider,
    String? baseUrl,
    String? model,
    String? apiKey,
    String? authType,
    int? maxTokens,
    String? systemPrompt,
  }) {
    return LlmConfig(
      provider: provider ?? this.provider,
      baseUrl: baseUrl ?? this.baseUrl,
      model: model ?? this.model,
      apiKey: apiKey ?? this.apiKey,
      authType: authType ?? this.authType,
      maxTokens: maxTokens ?? this.maxTokens,
      systemPrompt: systemPrompt ?? this.systemPrompt,
    );
  }

  Map<String, dynamic> toJson() => {
        'provider': provider,
        'base_url': baseUrl,
        'model': model,
        'api_key': apiKey,
        'auth_type': authType,
        'max_tokens': maxTokens,
        'system_prompt': systemPrompt,
      };

  factory LlmConfig.fromJson(Map<String, dynamic> json) {
    const fallback = LlmConfig();
    final rawTokens = json['max_tokens'];
    return LlmConfig(
      provider: (json['provider'] as String?) ?? fallback.provider,
      baseUrl: (json['base_url'] as String?) ?? fallback.baseUrl,
      model: (json['model'] as String?) ?? fallback.model,
      apiKey: (json['api_key'] as String?) ?? '',
      authType: (json['auth_type'] as String?) ?? fallback.authType,
      maxTokens: rawTokens is int
          ? rawTokens
          : int.tryParse('$rawTokens') ?? fallback.maxTokens,
      systemPrompt: (json['system_prompt'] as String?) ?? fallback.systemPrompt,
    );
  }
}

/// 一个服务商预设。[contextWindow] 只用于展示和体积预警，不进请求体。
class LlmPreset {
  const LlmPreset(
    this.key,
    this.label,
    this.baseUrl,
    this.authType,
    this.model, {
    this.maxTokens = defaultMaxTokens,
    this.contextWindow = 0,
  });

  final String key;
  final String label;
  final String baseUrl;
  final String authType;
  final String model;
  final int maxTokens;
  final int contextWindow;
}

/// 顺序即界面上的下拉顺序。
const List<LlmPreset> llmPresets = [
  LlmPreset('deepseek', 'DeepSeek V4 Pro', 'https://api.deepseek.com/v1',
      'bearer', 'deepseek-v4-pro', contextWindow: defaultContextWindow),
  LlmPreset('deepseek_flash', 'DeepSeek V4 Flash', 'https://api.deepseek.com/v1',
      'bearer', 'deepseek-v4-flash', contextWindow: defaultContextWindow),
  LlmPreset('openai', 'OpenAI', 'https://api.openai.com/v1', 'bearer',
      'gpt-4o-mini'),
  LlmPreset('bailian', '阿里百炼',
      'https://dashscope.aliyuncs.com/compatible-mode/v1', 'bearer', 'qwen-plus'),
  LlmPreset('kimi_global', 'Kimi（Global）', 'https://api.moonshot.ai/v1',
      'bearer', 'kimi-k2.6'),
  LlmPreset('kimi_cn', 'Kimi（CN）', 'https://api.moonshot.cn/v1', 'bearer',
      'kimi-k2.6'),
  LlmPreset('openai_compatible', 'OpenAI 兼容 API', 'https://api.openai.com/v1',
      'bearer', 'gpt-4o-mini'),
  LlmPreset('custom', '自定义', '', 'bearer', ''),
];

LlmPreset? presetFor(String provider) {
  for (final preset in llmPresets) {
    if (preset.key == provider) return preset;
  }
  return null;
}

String providerLabel(String provider) => presetFor(provider)?.label ?? provider;

/// 从 baseUrl + authType 反推服务商，用于打开设置页时选中正确的下拉项。
String inferProvider(LlmConfig config) {
  final target = _stripSlash(config.baseUrl);
  for (final preset in llmPresets) {
    if (preset.key == 'custom') continue;
    if (_stripSlash(preset.baseUrl) == target &&
        preset.authType == config.authType) {
      return preset.key;
    }
  }
  return 'custom';
}

/// 套用预设。[keepModel] 为 true 时保留用户已填的模型名。
LlmConfig applyPreset(LlmConfig config, String provider,
    {bool keepModel = false}) {
  final preset = presetFor(provider);
  if (preset == null || preset.key == 'custom') {
    return config.copyWith(provider: 'custom');
  }
  return config.copyWith(
    provider: provider,
    baseUrl: preset.baseUrl.isEmpty ? config.baseUrl : preset.baseUrl,
    model: keepModel && config.model.isNotEmpty ? config.model : preset.model,
    authType: preset.authType,
    maxTokens: config.maxTokens > 0 ? config.maxTokens : preset.maxTokens,
  );
}

String _stripSlash(String value) =>
    value.endsWith('/') ? value.substring(0, value.length - 1) : value;

/// 配置与历史的本地存储。全部留在手机上，不写设备端 NVS。
class ConfigStore {
  static const _configKey = 'llm_config';
  static const _historyKey = 'history';
  static const _historyLimit = 40;

  Future<LlmConfig> loadConfig() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_configKey);
    if (raw == null || raw.isEmpty) return const LlmConfig();
    try {
      final decoded = jsonDecode(raw);
      if (decoded is Map<String, dynamic>) return LlmConfig.fromJson(decoded);
    } on FormatException {
      // 配置损坏时用默认值继续，而不是让应用起不来。
    }
    return const LlmConfig();
  }

  Future<void> saveConfig(LlmConfig config) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_configKey, jsonEncode(config.toJson()));
  }

  /// 历史用于给 LLM 提供跨任务上下文，只留最近若干条。
  Future<List<Map<String, dynamic>>> loadHistory() async {
    final prefs = await SharedPreferences.getInstance();
    final items = prefs.getStringList(_historyKey) ?? const [];
    final out = <Map<String, dynamic>>[];
    for (final line in items) {
      try {
        final decoded = jsonDecode(line);
        if (decoded is Map<String, dynamic>) out.add(decoded);
      } on FormatException {
        continue;
      }
    }
    return out;
  }

  Future<void> appendHistory(Map<String, dynamic> item) async {
    final prefs = await SharedPreferences.getInstance();
    final items = prefs.getStringList(_historyKey) ?? <String>[];
    items.add(jsonEncode(item));
    if (items.length > _historyLimit) {
      items.removeRange(0, items.length - _historyLimit);
    }
    await prefs.setStringList(_historyKey, items);
  }

  Future<void> clearHistory() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_historyKey);
  }
}
