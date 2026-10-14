// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:convert';


import 'package:esp_claw_vibe/src/ble_bridge.dart';
import 'package:esp_claw_vibe/src/llm_client.dart';
import 'package:esp_claw_vibe/src/llm_config.dart';
import 'package:esp_claw_vibe/src/module_catalog.dart';
import 'package:esp_claw_vibe/src/playbook_registry.dart';
import 'package:esp_claw_vibe/src/task_planner.dart';
import 'package:esp_claw_vibe/src/vibe_session.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

/// 内存资产包。这样测试不依赖真实 assets 打包，也能跑在纯 Dart 环境里。
class FakeBundle extends CachingAssetBundle {
  FakeBundle(this.files);

  final Map<String, String> files;

  @override
  Future<ByteData> load(String key) async {
    final text = files[key];
    if (text == null) throw FlutterError('缺少资产：$key');
    return ByteData.sublistView(Uint8List.fromList(utf8.encode(text)));
  }
}

/// 假 bridge：按 op 回放固定响应，并记下收到的请求。
class FakeBridge implements Bridge {
  FakeBridge({required this.statuses});

  /// 每次 job_status 依次返回一条；用完后重复最后一条。
  final List<String> statuses;
  final List<String> ops = [];
  int _statusCalls = 0;

  @override
  Future<Map<String, dynamic>> request(
    Map<String, dynamic> command, {
    Duration? timeout,
  }) async {
    final op = '${command['op']}';
    ops.add(op);
    switch (op) {
      case 'put_lua':
        return {'id': command['id'], 'ok': true, 'data': '/scripts/host/t.lua'};
      case 'run_lua':
        return {'id': command['id'], 'ok': true, 'data': 'Lua job job0001'};
      case 'job_status':
        final index = _statusCalls < statuses.length
            ? _statusCalls
            : statuses.length - 1;
        _statusCalls++;
        return {'id': command['id'], 'ok': true, 'data': statuses[index]};
      case 'job_tail':
        return {'id': command['id'], 'ok': false, 'error': 'unsupported'};
      default:
        return {'id': command['id'], 'ok': true, 'data': ''};
    }
  }
}

/// 把脚本列表包装成流式 SSE 响应，按调用次序逐一返回。
LlmClient fakeLlm(List<String> scripts, {List<String>? seenBodies}) {
  var call = 0;
  final client = MockClient((request) async {
    seenBodies?.add(request.body);
    final script = scripts[call < scripts.length ? call : scripts.length - 1];
    call++;
    final content = '```lua\n$script\n```';
    final chunk = jsonEncode({
      'choices': [
        {'delta': {'content': content}, 'finish_reason': 'stop'}
      ]
    });
    return http.Response('data: $chunk\n\ndata: [DONE]\n\n', 200,
        headers: {'content-type': 'text/event-stream'});
  });
  return LlmClient(client: client);
}

const String _manifest = '{"module_docs":["lua_module_system"],'
    '"playbooks":["00_baseline.md"]}';

const String _catalog = '''
{
  "tier_labels": {"core": "基础设施"},
  "implied": {"lvgl": ["board_manager"]},
  "per_doc_limit": 12000,
  "total_limit": 26000,
  "modules": [
    {"require": "system", "component": "lua_module_system", "tier": "core",
     "summary": "系统状态查询", "keywords": ["内存", "system"]}
  ]
}
''';

const String _playbook = '---\n'
    '{"name":"baseline","description":"基线约束","always":true,"priority":0}\n'
    '---\n\n只用 API 参考里出现过的函数。\n';

Future<VibeSession> buildSession(LlmClient llm) async {
  final bundle = FakeBundle({
    'assets/catalog.json': _catalog,
    'assets/manifest.json': _manifest,
    'assets/playbooks/00_baseline.md': _playbook,
    'assets/lua_modules/lua_module_system.md': '# system\n\nsystem.heap()',
  });
  return VibeSession(
    catalog: await ModuleCatalog.load(bundle: bundle),
    registry: await PlaybookRegistry.load(bundle: bundle),
    llm: llm,
  );
}

const String _goodScript = 'local system = require("system")\nprint(system.heap())';

TaskPlan _plan({String taskKind = 'sensor'}) => TaskPlan(
      taskKind: taskKind,
      uiStack: 'none',
      modules: ['system'],
      source: 'test',
    );

void main() {
  const config = LlmConfig(apiKey: 'k', maxTokens: 2000);

  test('status=done 立刻停，只花一次 LLM 调用', () async {
    final bodies = <String>[];
    final session = await buildSession(fakeLlm([_goodScript], seenBodies: bodies));
    final bridge = FakeBridge(statuses: ['job_id=job0001\nstatus=done\nruntime_s=1']);

    final result = await session.iterate(
      bridge: bridge,
      config: config,
      prompt: '打印内存',
      plan: _plan(),
      maxAttempts: 3,
    );

    expect(result.attempts, hasLength(1));
    expect(result.attempts.single.ok, isTrue);
    expect(bodies, hasLength(1));
  });

  test('同一个脚本重复生成时提前停，不用满轮数', () async {
    final session = await buildSession(fakeLlm([_goodScript]));
    final bridge = FakeBridge(statuses: [
      'job_id=job0001\nstatus=failed\nruntime_s=1\nsummary=boom\nrecent_log=err A',
    ]);

    final result = await session.iterate(
      bridge: bridge,
      config: config,
      prompt: '打印内存',
      plan: _plan(),
      maxAttempts: 3,
    );

    expect(result.attempts, hasLength(2));
    expect(result.attempts.last.stoppedBecause, contains('实质相同'));
  });

  test('连续两轮同一个错误签名就停', () async {
    final session = await buildSession(fakeLlm([
      _goodScript,
      '$_goodScript\nprint("v2")',
      '$_goodScript\nprint("v3")',
    ]));
    final bridge = FakeBridge(statuses: [
      'job_id=job0001\nstatus=failed\nsummary=x\nrecent_log=attempt to call a nil value',
    ]);

    final result = await session.iterate(
      bridge: bridge,
      config: config,
      prompt: '打印内存',
      plan: _plan(),
      maxAttempts: 4,
    );

    expect(result.attempts, hasLength(2));
    expect(result.attempts.last.stoppedBecause, contains('没有收敛'));
  });

  test('设备没给诊断信息就不再重试', () async {
    final session = await buildSession(fakeLlm([_goodScript]));
    final bridge = FakeBridge(statuses: ['job_id=job0001\nstatus=failed']);

    final result = await session.iterate(
      bridge: bridge,
      config: config,
      prompt: '打印内存',
      plan: _plan(),
      maxAttempts: 3,
    );

    expect(result.attempts, hasLength(1));
    expect(result.attempts.single.stoppedBecause, contains('缺少可用于修脚本的信息'));
  });

  test('常驻任务 running 够久算成功', () async {
    const uiScript = 'local lvgl = require("lvgl")\n'
        'lvgl.create_screen()\n'
        'while true do lvgl.process_events(16) end';
    final session = await buildSession(fakeLlm([uiScript]));
    final bridge =
        FakeBridge(statuses: ['job_id=job0001\nstatus=running\nruntime_s=30']);

    final result = await session.iterate(
      bridge: bridge,
      config: config,
      prompt: '做一个贪吃蛇游戏',
      plan: _plan(taskKind: 'game'),
      maxAttempts: 3,
      pollTimeout: Duration.zero,
    );

    expect(result.attempts, hasLength(1));
    expect(result.attempts.single.ok, isTrue);
    // 常驻任务必须要到不限时窗口，否则玩着玩着就被设备掐掉。
    expect(bridge.ops, contains('run_lua'));
  });

  test('本地校验拦下不完整脚本，不上传设备', () async {
    final session = await buildSession(fakeLlm(['local x = 1']));
    final bridge = FakeBridge(statuses: ['job_id=job0001\nstatus=done']);

    final result = await session.iterate(
      bridge: bridge,
      config: config,
      prompt: '打印内存',
      plan: _plan(),
      maxAttempts: 1,
    );

    expect(result.attempts.single.error, contains('require'));
    expect(bridge.ops, isEmpty);
  });
}
