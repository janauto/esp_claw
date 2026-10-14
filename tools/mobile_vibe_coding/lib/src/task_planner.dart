// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:convert';

import 'constants.dart';
import 'module_catalog.dart';

const List<String> uiStacks = ['lvgl', 'display', 'none'];
const List<String> taskKinds = [
  'game', 'ui', 'sensor', 'control', 'io', 'net', 'media', 'mixed',
];

/// 实现计划里的一步。
class PlanStep {
  const PlanStep({required this.order, required this.action, this.modules = const []});

  final int order;
  final String action;
  final List<String> modules;

  String describe() {
    final mods = modules.isEmpty ? '' : '（${modules.join(', ')}）';
    return '$order. $action$mods';
  }
}

/// 规划阶段的完整产出。可变，因为 docs 要在加载 README 后回填。
class TaskPlan {
  TaskPlan({
    this.goal = '',
    this.taskKind = '',
    this.uiStack = 'none',
    this.inputMethod = 'none',
    List<String>? modules,
    List<PlanStep>? steps,
    List<String>? risks,
    List<String>? acceptance,
    this.notes = '',
    this.source = 'keywords',
    List<String>? docs,
    List<String>? playbooks,
  })  : modules = modules ?? [],
        steps = steps ?? [],
        risks = risks ?? [],
        acceptance = acceptance ?? [],
        docs = docs ?? [],
        playbooks = playbooks ?? [];

  String goal;
  String taskKind;
  String uiStack;
  String inputMethod;
  List<String> modules;
  List<PlanStep> steps;
  List<String> risks;
  List<String> acceptance;
  String notes;
  String source;
  List<String> docs;
  List<String> playbooks;

  String describe() {
    final lines = <String>['规划（$source）'];
    if (goal.isNotEmpty) lines.add('  目标：$goal');
    final meta = <String>['类型=${taskKind.isEmpty ? '未分类' : taskKind}'];
    if (uiStack != 'none') meta.add('界面栈=$uiStack');
    if (inputMethod != 'none') meta.add('输入=$inputMethod');
    lines.add('  ${meta.join('，')}');
    lines.add('  模块：${modules.isEmpty ? '(无)' : modules.join(', ')}');
    if (steps.isNotEmpty) {
      lines.add('  步骤：');
      lines.addAll(steps.map((s) => '    ${s.describe()}'));
    }
    if (risks.isNotEmpty) lines.add('  风险：${risks.join('；')}');
    if (acceptance.isNotEmpty) lines.add('  验收：${acceptance.join('；')}');
    if (playbooks.isNotEmpty) lines.add('  Playbook：${playbooks.join(', ')}');
    if (docs.isNotEmpty) lines.add('  载入 README：${docs.join(', ')}');
    if (notes.isNotEmpty) lines.add('  说明：$notes');
    return lines.join('\n');
  }

  /// 规划结果转成生成阶段的上下文段落。
  String asContext() {
    final lines = <String>[];
    if (goal.isNotEmpty) lines.add('**目标**：$goal');
    if (taskKind.isNotEmpty) lines.add('**任务类型**：$taskKind');
    if (uiStack != 'none') {
      final other = uiStack == 'display' ? 'lvgl' : 'display';
      lines.add('**界面栈**：只用 `$uiStack`，本脚本内不要 require("$other")。');
    }
    if (inputMethod != 'none') lines.add('**输入方式**：$inputMethod');
    if (modules.isNotEmpty) {
      lines.add('**允许 require 的模块**：${modules.join(', ')}');
    }
    if (steps.isNotEmpty) {
      lines.add('\n**实现步骤**（按此顺序落地，不要跳步）：');
      lines.addAll(steps.map((s) => s.describe()));
    }
    if (risks.isNotEmpty) {
      lines.add('\n**已预判的风险，生成时必须规避**：');
      lines.addAll(risks.map((r) => '- $r'));
    }
    if (acceptance.isNotEmpty) {
      lines.add('\n**验收标准**：');
      lines.addAll(acceptance.map((a) => '- $a'));
    }
    return lines.join('\n');
  }
}

const String plannerSystemPrompt =
    '''你是 ESP-Claw 设备端 Lua 脚本的实现规划器。用户会描述一个想在设备上跑起来的功能，你负责把它拆成一份可直接落地的实现计划。你不写代码。

只输出一个 JSON 对象，不要 markdown 代码块，不要任何解释文字。字段：
{
  "goal": "一句话说明脚本跑起来后用户会看到/得到什么",
  "task_kind": "game|ui|sensor|control|io|net|media|mixed 之一",
  "ui_stack": "lvgl|display|none 之一",
  "input_method": "lcd_touch|button|knob|imu|adc|none 之一",
  "modules": ["只能取自下方模块清单里出现过的名字"],
  "steps": [{"order":1,"action":"这一步具体做什么","modules":["本步用到的模块"]}],
  "risks": ["这类任务在本设备上最容易踩的坑，每条一句话"],
  "acceptance": ["怎么判断脚本真的成功了，要能从设备日志或屏幕上观察到"]
}

规划规则：
1. modules 只能来自清单，绝不虚构；一般 3-7 个，宁缺勿滥。
2. ui_stack 二选一，绝不同时用 display 和 lvgl。**默认选 lvgl**：
   - 控件界面（按钮/标签/滑块/列表）、触摸事件回调 -> lvgl
   - 游戏和动画也用 lvgl：画面元素做成 lvgl 对象，用 set_pos 增量移动，
     输入走 on("clicked") 事件回调。比逐帧全屏重绘更稳，且不受 ASCII 限制。
     跳跃闯关、贪吃蛇、俄罗斯方块、打砖块这类都能用 lvgl 对象拼出来，一律选 lvgl。
   - 只有确实需要逐像素自绘、画面无法用对象表达时才选 display。
   - 纯数据采集、纯 IO 控制、不上屏 -> none
3. 用到屏幕必须带 board_manager；有主循环/动画节拍/需要保持画面必须带 delay。
4. steps 要覆盖完整生命周期：初始化 -> 主逻辑 -> 收尾/保持画面。
   游戏类必须显式包含状态初始化、输入采样、状态更新、渲染、结束判定这几步。
5. risks 要具体到 API 层面，不要写"注意性能"这种空话。
   例如"lvgl 颜色是 0xRRGGBB 数字而 display 是字符串"、"set_pos 坐标必须取整"、
   "不要用 get_pos 读回坐标做增量"、"get_display_lcd_params 是多返回值"。
6. 设备是单片机：内存有限、没有协程调度器、脚本跑在一个 Lua job 里。
   不要规划需要多进程、大内存缓冲或外部网络依赖的方案。
''';

const String plannerSchemaHint = '上一次返回不符合要求：{error}\n'
    '请重新只输出一个合法 JSON 对象，字段和取值范围严格按系统提示。';

/// 从 LLM 回复里取出第一个 JSON 对象，容忍 ```json 包裹和前后闲聊。
Map<String, dynamic> extractJsonObject(String text) {
  var stripped = text.trim();
  final fence = RegExp(r'```(?:json)?\s*([\s\S]*?)```', caseSensitive: false)
      .firstMatch(stripped);
  if (fence != null) stripped = fence.group(1)!.trim();

  final start = stripped.indexOf('{');
  final end = stripped.lastIndexOf('}');
  if (start < 0 || end <= start) {
    throw const FormatException('回复里没有 JSON 对象');
  }
  final decoded = jsonDecode(stripped.substring(start, end + 1));
  if (decoded is! Map<String, dynamic>) {
    throw const FormatException('JSON 顶层不是对象');
  }
  return decoded;
}

List<String> _coerceStrList(Object? raw, {int limit = 8}) {
  if (raw is! List) return const [];
  final out = <String>[];
  for (final entry in raw) {
    if (entry is! String) continue;
    final value = entry.trim();
    if (value.isEmpty) continue;
    out.add(value);
    if (out.length >= limit) break;
  }
  return out;
}

String _coerceEnum(Object? raw, List<String> allowed, String fallback) {
  final value = raw is String ? raw.trim().toLowerCase() : '';
  return allowed.contains(value) ? value : fallback;
}

List<PlanStep> _coerceSteps(Object? raw) {
  if (raw is! List) return const [];
  final out = <PlanStep>[];
  for (final entry in raw) {
    if (entry is! Map) continue;
    final action = (entry['action'] as String?)?.trim() ?? '';
    if (action.isEmpty) continue;
    final rawOrder = entry['order'];
    final order = rawOrder is int
        ? rawOrder
        : int.tryParse('$rawOrder') ?? out.length + 1;
    out.add(PlanStep(
      order: order,
      action: action,
      modules: _coerceStrList(entry['modules'], limit: 6),
    ));
    if (out.length >= 12) break;
  }
  return out;
}

/// display 和 lvgl 互斥，按选定的界面栈摘掉另一个。
List<String> _resolveUiConflict(List<String> modules, String uiStack) {
  if (uiStack == 'lvgl') {
    return modules.where((m) => m != 'display').toList();
  }
  if (uiStack == 'display') {
    return modules.where((m) => m != 'lvgl').toList();
  }
  return modules;
}

/// 把 LLM 的 JSON 转成 TaskPlan，顺带做取值域校验。schema 不合规时抛异常。
TaskPlan parsePlan(
  Map<String, dynamic> payload,
  ModuleCatalog catalog, {
  String source = 'llm',
}) {
  var modules = catalog.normalizeRequires(payload['modules']);
  if (modules.isEmpty) {
    throw const FormatException('modules 为空或全部不在模块清单里');
  }

  var uiStack = _coerceEnum(payload['ui_stack'], uiStacks, 'none');
  // 模型有时选了 lvgl/display 却漏填 ui_stack，从 modules 反推补齐。
  if (uiStack == 'none') {
    if (modules.contains('lvgl')) {
      uiStack = 'lvgl';
    } else if (modules.contains('display')) {
      uiStack = 'display';
    }
  }

  modules = catalog.expandWithImplied(_resolveUiConflict(modules, uiStack));

  return TaskPlan(
    goal: ((payload['goal'] as String?) ?? '').trim(),
    taskKind: _coerceEnum(payload['task_kind'], taskKinds, 'mixed'),
    uiStack: uiStack,
    inputMethod:
        ((payload['input_method'] as String?) ?? 'none').trim().toLowerCase(),
    modules: modules,
    steps: _coerceSteps(payload['steps']),
    risks: _coerceStrList(payload['risks']),
    acceptance: _coerceStrList(payload['acceptance'], limit: 6),
    notes: ((payload['notes'] as String?) ?? '').trim(),
    source: source,
  );
}

/// 「点击屏幕」这类说法意味着触摸输入，但 lvgl 分支会把 lcd_touch 从 modules 里摘掉。
const List<String> _touchHints = [
  '点击', '触摸', '触屏', '点按', 'touch', 'tap', 'click',
];

/// task_kind 关键词表。顺序即优先级：命中靠前的类别先赢，
/// 避免「贪吃蛇游戏」被判成 ui。
const List<({String kind, List<String> keywords})> _taskKindHints = [
  (kind: 'game', keywords: [
    '游戏', 'game', '贪吃蛇', 'snake', '俄罗斯方块', 'tetris',
    '打砖块', 'breakout', '跳跃', 'jump', '得分', 'score', '关卡', '障碍',
  ]),
  (kind: 'sensor', keywords: [
    '传感器', 'sensor', '温度', '湿度', '加速度', '陀螺仪', 'imu',
    '采集', '读取数据', 'adc', '光照',
  ]),
  (kind: 'control', keywords: [
    '电机', 'motor', '舵机', 'servo', 'pwm', 'mcpwm', '控制转速', '占空比', '驱动',
  ]),
  (kind: 'io', keywords: [
    'gpio', '引脚', '电平', 'i2c', 'spi', 'uart', '串口', '继电器',
  ]),
  (kind: 'net', keywords: [
    'wifi', '网络', 'http', 'mqtt', '联网', '请求', '下载', '上传',
  ]),
  (kind: 'media', keywords: [
    '音频', '播放', '录音', 'audio', '喇叭', '麦克风', '摄像头', '拍照',
  ]),
  (kind: 'ui', keywords: [
    '界面', '按钮', '菜单', '列表', '显示', '屏幕', 'ui', 'label', '文字', '图片', '动画',
  ]),
];

/// modules 到 task_kind 的兜底映射，用于提示词没有明显类别词的情况。
const List<({String module, String kind})> _moduleKindHints = [
  (module: 'imu', kind: 'sensor'),
  (module: 'adc', kind: 'sensor'),
  (module: 'mcpwm', kind: 'control'),
  (module: 'gpio', kind: 'io'),
  (module: 'i2c', kind: 'io'),
  (module: 'uart', kind: 'io'),
  (module: 'lvgl', kind: 'ui'),
  (module: 'display', kind: 'ui'),
];

/// 按关键词推断 task_kind，失败时回退到 modules 映射。
///
/// task_kind 是 playbook 匹配的主维度，留空会让所有带 task_kind 条件的
/// playbook 全部落空，只剩 baseline 生效。
String inferTaskKind(String prompt, List<String> modules) {
  final haystack = prompt.toLowerCase();
  for (final hint in _taskKindHints) {
    if (hint.keywords.any((kw) => haystack.contains(kw.toLowerCase()))) {
      return hint.kind;
    }
  }
  for (final hint in _moduleKindHints) {
    if (modules.contains(hint.module)) return hint.kind;
  }
  return '';
}

/// LLM 规划不可用时的关键词兜底。只保证模块选择可用，没有步骤和风险。
TaskPlan fallbackPlan(String prompt, ModuleCatalog catalog, {String reason = ''}) {
  final modules = catalog.selectByKeywords(prompt);
  final uiStack = modules.contains('lvgl')
      ? 'lvgl'
      : (modules.contains('display') ? 'display' : 'none');
  var inputMethod = 'none';
  for (final candidate in ['lcd_touch', 'button', 'knob', 'imu', 'adc']) {
    if (modules.contains(candidate)) {
      inputMethod = candidate;
      break;
    }
  }
  // lvgl 内建触摸处理，selectByKeywords 会移除 lcd_touch；这里按提示词补回输入方式，
  // 否则 inputMethod 恒为 none，playbook 和提示词都拿不到输入相关约束。
  if (inputMethod == 'none' && uiStack == 'lvgl') {
    final haystack = prompt.toLowerCase();
    if (_touchHints.any(haystack.contains)) inputMethod = 'lcd_touch';
  }
  return TaskPlan(
    taskKind: inferTaskKind(prompt, modules),
    modules: modules,
    uiStack: uiStack,
    inputMethod: inputMethod,
    source: 'keywords',
    notes: reason,
  );
}

/// 规划入口。[callLlm] 由调用方注入，这样规划器不依赖具体 HTTP 客户端，
/// 也便于单测里替换成假实现。
Future<TaskPlan> planTask(
  String prompt,
  ModuleCatalog catalog,
  Future<String> Function(List<Map<String, String>> messages, int maxTokens) callLlm, {
  String deviceInfo = '',
}) async {
  final baseMessages = <Map<String, String>>[
    {'role': 'system', 'content': plannerSystemPrompt},
    {'role': 'system', 'content': '可用模块清单：\n${catalog.overview()}'},
  ];
  if (deviceInfo.isNotEmpty) {
    baseMessages.add({'role': 'system', 'content': '目标设备信息：\n$deviceInfo'});
  }
  baseMessages.add({'role': 'user', 'content': prompt});

  var messages = List<Map<String, String>>.from(baseMessages);
  var lastError = '';
  var lastRaw = '';

  // 两次机会：第一次直出，不合规就把模型自己的输出和校验错误一起回灌重试一次。
  for (var attempt = 0; attempt < 2; attempt++) {
    try {
      lastRaw = await callLlm(messages, plannerMaxTokens);
      final plan = parsePlan(extractJsonObject(lastRaw), catalog);
      if (lastError.isNotEmpty) {
        plan.notes = '${plan.notes}（首次规划不合规已重试：$lastError）'.trim();
      }
      return plan;
    } on Object catch (err) {
      lastError = '$err';
      if (attempt == 0) {
        messages = [
          ...baseMessages,
          {'role': 'assistant', 'content': lastRaw.isEmpty ? '(空回复)' : lastRaw},
          {
            'role': 'user',
            'content': plannerSchemaHint.replaceFirst('{error}', lastError),
          },
        ];
      }
    }
  }

  return fallbackPlan(prompt, catalog,
      reason: 'LLM 规划不可用，已退回关键词匹配：$lastError');
}
