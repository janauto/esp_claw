// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0
import 'constants.dart';
import 'context_engine.dart';
import 'module_catalog.dart';
import 'playbook_registry.dart';
const String outputContract = '''你为 ESP-Claw 设备生成可直接在设备 Lua runtime 执行的脚本。
**输出格式**：只返回一个 ```lua 代码块，块内是完整可运行的脚本。不要在代码块外写任何解释、说明或步骤描述。不要返回多个代码块。''';
/// 输出契约 + 生成规则。优先级最高，永不被预算裁掉。
ContextProvider systemRulesProvider([String extra = '']) {
  return ContextProvider(
    name: 'system_rules',
    stable: true,
    collect: (request) async {
      final parts = <String>[outputContract];
      if (extra.trim().isNotEmpty) parts.add(extra.trim());
      return ContextSection(
        name: '输出要求与生成规则',
        kind: ContextKind.system,
        text: parts.join('\n\n'),
        priority: 1,
      );
    },
  );
}
ContextProvider deviceProvider() {
  return ContextProvider(
    name: 'device',
    stable: true,
    collect: (request) async {
      if (request.deviceInfo.trim().isEmpty) return null;
      return ContextSection(
        name: '目标设备',
        kind: ContextKind.system,
        text: request.deviceInfo.trim(),
        priority: 10,
      );
    },
  );
}
/// 按规划结果挑 playbook 并注入全文。
ContextProvider playbookProvider(PlaybookRegistry registry, ModuleCatalog catalog) {
  return ContextProvider(
    name: 'playbook',
    stable: true,
    collect: (request) async {
      if (registry.playbooks.isEmpty) return null;
      final plan = request.plan;
      String taskKind;
      String uiStack;
      List<String> modules;
      if (plan != null) {
        taskKind = plan.taskKind;
        uiStack = plan.uiStack;
        modules = plan.modules;
      } else {
        // 规划调用失败时和 api_reference 用同一套兜底，避免两个 provider 降级不一致。
        // taskKind 无法从关键词可靠推断，留空——只按模块匹配的 playbook 仍能入选。
        modules = catalog.selectByKeywords(request.prompt);
        uiStack = modules.contains('lvgl')
            ? 'lvgl'
            : (modules.contains('display') ? 'display' : 'none');
        taskKind = '';
      }
      final picked = registry.select(
        taskKind: taskKind,
        uiStack: uiStack,
        modules: modules,
      );
      if (picked.isEmpty) return null;
      if (plan != null) {
        plan.playbooks = picked.map((pb) => pb.name).toList();
      }
      final body = PlaybookRegistry.render(picked);
      if (body.isEmpty) return null;
      return ContextSection(
        name: '实现规范',
        kind: ContextKind.system,
        text: body,
        priority: 15,
      );
    },
  );
}
/// 加载选中模块的 README 全文。这是最大的一段，但也是准确性的根本来源。
ContextProvider apiReferenceProvider(ModuleCatalog catalog) {
  return ContextProvider(
    name: 'api_reference',
    stable: true,
    collect: (request) async {
      final plan = request.plan;
      final modules =
          plan != null ? plan.modules : catalog.selectByKeywords(request.prompt);
      if (modules.isEmpty) return null;
      final loaded = await catalog.loadModuleDocs(modules);
      if (loaded.context.trim().isEmpty) return null;
      if (plan != null) {
        plan.docs = loaded.docs;
      }
      return ContextSection(
        name: '模块 API 参考',
        kind: ContextKind.system,
        text: loaded.context,
        priority: 20,
      );
    },
  );
}
ContextProvider planProvider() {
  return ContextProvider(
    name: 'plan',
    stable: true,
    collect: (request) async {
      final plan = request.plan;
      if (plan == null) return null;
      final body = plan.asContext();
      if (body.trim().isEmpty) return null;
      return ContextSection(
        name: '实现计划',
        kind: ContextKind.system,
        text: body,
        priority: 5,
      );
    },
  );
}
/// 把设备端报错翻译成定向修复指引。
///
/// 只匹配设备真实输出的英文 Lua 错误文本——中文正则匹配的是需求描述，
/// 对设备日志基本不会命中。
String diagnose(String summary, [String log = '']) {
  final blob = '$summary\n$log';
  final hints = <String>[];
  final nilCall = RegExp(
          r"attempt to call a nil value \((?:field|global|method) '([\w.]+)'\)")
      .firstMatch(blob);
  if (nilCall != null) {
    hints.add('`${nilCall.group(1)}` 不存在。回到上面的 API 参考确认该模块到底有哪些函数，'
        '找不到就说明这个 API 没有，必须换已存在的 API 实现，不要改个名字再猜一次。');
  }
  final nilIndex = RegExp(
          r"attempt to index a nil value \((?:field|global|local) '(\w+)'\)")
      .firstMatch(blob);
  if (nilIndex != null) {
    hints.add('`${nilIndex.group(1)}` 是 nil。常见原因：模块没 require、句柄获取失败，'
        '或者把多返回值函数（如 `board_manager.get_display_lcd_params`）当成 table 接收了。');
  }
  if (blob.contains('handle expected') ||
      RegExp(r'bad argument #1.*(?:userdata|handle)').hasMatch(blob)) {
    hints.add('缺少句柄参数。`lcd_touch.*`、`display.init` 这类函数的句柄必须先从 '
        '`board_manager.get_*` 取到再传进去。');
  }
  if (RegExp(r'number has no integer representation|bad argument.*number expected.*got number')
      .hasMatch(blob)) {
    hints.add('传了浮点数给要求整数的参数。display 的坐标/宽高/半径/font_size 不会自动取整，'
        '所有除法改用 `//`，或用 `math.floor()` 显式转整。');
  }
  final badArg =
      RegExp(r"bad argument #(\d+) to '(\w+)' \(([^)]+)\)").firstMatch(blob);
  if (badArg != null && hints.isEmpty) {
    hints.add('`${badArg.group(2)}` 的第 ${badArg.group(1)} 个参数不对（${badArg.group(3)}）。'
        '回到 API 参考核对完整签名，特别是参数个数。');
  }
  final missing = RegExp(r"module '(\w+)' not found").firstMatch(blob);
  if (missing != null) {
    hints.add('模块 `${missing.group(1)}` 在本设备上不存在，只能 require 计划里列出的模块。');
  }
  if (blob.contains('not enough memory') || blob.contains('out of memory')) {
    hints.add('内存不足。缩小缓冲区、降低分辨率或改成逐行/分块处理，不要一次分配整屏缓冲。');
  }
  if (hints.isEmpty) return '';
  return hints.map((h) => '- $h').join('\n');
}
/// 针对最近一次失败给出定向诊断。动态段，每轮重收。
ContextProvider diagnosisProvider() {
  return ContextProvider(
    name: 'diagnosis',
    collect: (request) async {
      if (request.attempts.isEmpty) return null;
      for (final attempt in request.attempts.reversed) {
        final outcome = attempt.outcome;
        if (outcome == null || outcome.ok) continue;
        final hints = diagnose(outcome.summary, outcome.log);
        if (hints.isNotEmpty) {
          return ContextSection(
            name: '上一轮失败的定向诊断',
            kind: ContextKind.system,
            text: hints,
            priority: 2,
          );
        }
        break;
      }
      return null;
    },
  );
}
/// 跨任务的会话历史。优先级最低，预算紧张时先丢它。
ContextProvider historyProvider({int limit = 4}) {
  return ContextProvider(
    name: 'history',
    collect: (request) async {
      final messages = <Map<String, String>>[];
      final recent = request.history.length > limit
          ? request.history.sublist(request.history.length - limit)
          : request.history;
      for (final item in recent) {
        final prompt = item['prompt'];
        final result = item['result'];
        if (prompt is String && prompt.trim().isNotEmpty) {
          messages.add({'role': 'user', 'content': prompt});
        }
        if (result is String && result.trim().isNotEmpty) {
          final clipped =
              result.length > 1500 ? result.substring(0, 1500) : result;
          messages.add({'role': 'assistant', 'content': '上次执行结果:\n$clipped'});
        }
      }
      if (messages.isEmpty) return null;
      return ContextSection(
        name: '会话历史',
        kind: ContextKind.messages,
        messages: messages,
        priority: 90,
      );
    },
  );
}
/// 把本任务前几轮的脚本原文和真实报错成对回灌。
///
/// 这是让模型能"只改出错那部分"的唯一途径：它必须看到自己上一轮写了什么。
/// 本地校验失败（attempt.error）和设备端运行失败（attempt.outcome）都要覆盖，
/// 否则本地校验挂掉的那轮反馈为空，模型会原样再生成一次，直接触发去重退出。
ContextProvider repairProvider() {
  return ContextProvider(
    name: 'repair',
    collect: (request) async {
      final attempts =
          request.attempts.where((a) => a.script.isNotEmpty).toList();
      if (attempts.isEmpty) return null;
      final recent = attempts.length > maxRepairRounds
          ? attempts.sublist(attempts.length - maxRepairRounds)
          : attempts;
      final messages = <Map<String, String>>[];
      for (final attempt in recent) {
        final script = attempt.script.length > maxScriptEcho
            ? attempt.script.substring(0, maxScriptEcho)
            : attempt.script;
        messages.add({'role': 'assistant', 'content': '```lua\n$script\n```'});
        final outcome = attempt.outcome;
        String detail;
        if (attempt.error.isNotEmpty) {
          detail = '第 ${attempt.index} 轮没有通过上位机本地校验。\n\n**校验错误**：${attempt.error}';
        } else if (outcome != null && outcome.flickered) {
          // 设备端不报错：脚本"正常"跑完退出了。但画面随之销毁，用户看到的是闪一下
          // 就回主界面。必须把这件事讲透，否则模型只会重新交一份同样没有主循环的代码。
          detail = '第 ${attempt.index} 轮在设备上跑完就退出了'
              '（status=done，仅运行 ${outcome.runtimeS}s），**这是失败**。\n\n'
              '**现象**：屏幕闪一下就回到主界面，设备日志没有任何输出。\n'
              '**原因**：脚本只创建了 LVGL 对象／定义了状态表，就走到了文件末尾。'
              'Lua 解释器一退出，屏幕上的对象全部销毁。\n\n'
              '**必须满足**：\n'
              '- 脚本末尾要有常驻主循环：`while true do ... lvgl.process_events(16) end`；\n'
              '- 所有游戏逻辑（移动、碰撞、计分）都放在这个循环里推进；\n'
              '- 输出完整可运行脚本，不要只给状态表或片段。';
        } else if (outcome != null && !outcome.ok) {
          detail = '第 ${attempt.index} 轮在设备上执行失败。\n\n**状态**：${outcome.status}';
          if (outcome.summary.isNotEmpty) {
            detail += '\n**错误摘要**：${outcome.summary}';
          }
          final hints = diagnose(outcome.summary, outcome.log);
          if (hints.isNotEmpty) detail += '\n\n**问题分析**：\n$hints';
          if (outcome.log.isNotEmpty) {
            final log = outcome.log.length > maxLogEcho
                ? outcome.log.substring(0, maxLogEcho)
                : outcome.log;
            detail += '\n\n**设备日志**：\n```\n$log\n```';
          }
        } else {
          continue;
        }
        messages.add({
          'role': 'user',
          'content': '$detail\n\n'
              '**修复要求**：\n'
              '1. 只改导致这个错误的部分，其余保持原样，不要重写整个脚本。\n'
              '2. 改之前回到 API 参考核对函数签名，不要凭记忆。\n'
              '3. 不要提交和上一轮实质相同的代码。\n'
              '4. 仍然只返回一个 lua 代码块。',
        });
      }
      if (messages.isEmpty) return null;
      return ContextSection(
        name: '修复历史',
        kind: ContextKind.messages,
        messages: messages,
        priority: 30,
      );
    },
  );
}
/// 按 stable -> dynamic 顺序装配好的 builder。
ContextBuilder buildDefaultBuilder({
  required ModuleCatalog catalog,
  required PlaybookRegistry registry,
  String systemPrompt = '',
  int? charBudget,
}) {
  return ContextBuilder(charBudget: charBudget ?? defaultCharBudget)
    ..registerAll([
      systemRulesProvider(systemPrompt),
      deviceProvider(),
      planProvider(),
      playbookProvider(registry, catalog),
      apiReferenceProvider(catalog),
      diagnosisProvider(),
      historyProvider(),
      repairProvider(),
    ]);
}
