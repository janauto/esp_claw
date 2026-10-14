// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'package:flutter/material.dart';

import '../app_state.dart';
import '../constants.dart';
import '../llm_config.dart';

/// LLM 配置页。API Key 只存在手机本地，不会写设备端 NVS。
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late AppState _state;
  late String _provider;
  late TextEditingController _baseUrl;
  late TextEditingController _model;
  late TextEditingController _apiKey;
  late TextEditingController _maxTokens;
  late TextEditingController _systemPrompt;
  late String _authType;
  bool _showKey = false;
  bool _initialized = false;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_initialized) return;
    _initialized = true;
    _state = AppStateScope.of(context);
    final config = _state.config;
    _provider = inferProvider(config);
    _baseUrl = TextEditingController(text: config.baseUrl);
    _model = TextEditingController(text: config.model);
    _apiKey = TextEditingController(text: config.apiKey);
    _maxTokens = TextEditingController(text: '${config.maxTokens}');
    _systemPrompt = TextEditingController(text: config.systemPrompt);
    _authType = config.authType;
  }

  @override
  void dispose() {
    _baseUrl.dispose();
    _model.dispose();
    _apiKey.dispose();
    _maxTokens.dispose();
    _systemPrompt.dispose();
    super.dispose();
  }

  void _applyPreset(String provider) {
    final next = applyPreset(_state.config, provider);
    setState(() {
      _provider = provider;
      if (provider != 'custom') {
        _baseUrl.text = next.baseUrl;
        _model.text = next.model;
        _authType = next.authType;
      }
    });
  }

  Future<void> _save() async {
    final tokens = int.tryParse(_maxTokens.text.trim()) ?? defaultMaxTokens;
    await _state.saveConfig(LlmConfig(
      provider: _provider,
      baseUrl: _baseUrl.text.trim(),
      model: _model.text.trim(),
      apiKey: _apiKey.text.trim(),
      authType: _authType,
      maxTokens: tokens < 1 ? defaultMaxTokens : tokens,
      systemPrompt: _systemPrompt.text.trim(),
    ));
    if (mounted) Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final preset = presetFor(_provider);
    return Scaffold(
      appBar: AppBar(
        title: const Text('LLM 设置'),
        actions: [
          TextButton(onPressed: _save, child: const Text('保存')),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          DropdownButtonFormField<String>(
            initialValue: _provider,
            decoration: const InputDecoration(
              labelText: '服务商',
              border: OutlineInputBorder(),
            ),
            items: llmPresets
                .map((p) => DropdownMenuItem(value: p.key, child: Text(p.label)))
                .toList(),
            onChanged: (value) {
              if (value != null) _applyPreset(value);
            },
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _baseUrl,
            decoration: const InputDecoration(
              labelText: 'Base URL',
              hintText: 'https://api.deepseek.com/v1',
              border: OutlineInputBorder(),
            ),
            keyboardType: TextInputType.url,
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _model,
            decoration: const InputDecoration(
              labelText: '模型',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _apiKey,
            obscureText: !_showKey,
            decoration: InputDecoration(
              labelText: 'API Key',
              border: const OutlineInputBorder(),
              suffixIcon: IconButton(
                icon: Icon(_showKey ? Icons.visibility_off : Icons.visibility),
                onPressed: () => setState(() => _showKey = !_showKey),
              ),
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            'API Key 只存在这台手机上，不会上传到设备，也不会写设备端 NVS。',
            style: TextStyle(fontSize: 12),
          ),
          const SizedBox(height: 16),
          DropdownButtonFormField<String>(
            initialValue: _authType,
            decoration: const InputDecoration(
              labelText: '鉴权方式',
              border: OutlineInputBorder(),
            ),
            items: const [
              DropdownMenuItem(value: 'bearer', child: Text('Bearer Token')),
              DropdownMenuItem(value: 'none', child: Text('无鉴权（本地服务）')),
            ],
            onChanged: (value) {
              if (value != null) setState(() => _authType = value);
            },
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _maxTokens,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
              labelText: 'max_tokens',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 8),
          Text(
            '一份完整的 LVGL 游戏脚本 200+ 行，给太小会被截断成半个程序——'
            '语法能过但一上屏就退。常驻类任务会自动提到至少 $longRunMinTokens。'
            '${preset != null && preset.contextWindow > 0 ? '当前模型上下文窗口约 ${preset.contextWindow ~/ 1000}k。' : ''}',
            style: const TextStyle(fontSize: 12),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _systemPrompt,
            minLines: 2,
            maxLines: 4,
            decoration: const InputDecoration(
              labelText: '附加系统提示',
              border: OutlineInputBorder(),
              alignLabelWithHint: true,
            ),
          ),
          const Divider(height: 32),
          ListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('清空会话历史'),
            subtitle: const Text('历史会作为跨任务上下文交给 LLM'),
            trailing: const Icon(Icons.delete_outline),
            onTap: () async {
              await ConfigStore().clearHistory();
              _state.log('已清空会话历史');
            },
          ),
        ],
      ),
    );
  }
}
