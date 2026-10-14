// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:convert';

import 'package:flutter/services.dart';

/// 一个可 require 的 Lua 模块。字段与 host_vibe_coding/lua_module_catalog.py
/// 的 LuaModuleInfo 对应，数据由 tool/sync_assets.py 导出。
class LuaModuleInfo {
  const LuaModuleInfo({
    required this.require,
    required this.component,
    required this.tier,
    required this.summary,
    required this.keywords,
  });

  final String require;
  final String component;
  final String tier;
  final String summary;
  final List<String> keywords;

  /// assets 里的 README 路径。多个 require 可能共享同一个组件文档。
  String get docAsset => 'assets/lua_modules/$component.md';
}

/// 两阶段上下文的第一阶段数据源：模块清单、隐含依赖、README 加载。
///
/// 全部 README 合计约 200 KB，一次性塞进请求既浪费 token 又会淹没关键 API 约束；
/// 按需加载可以让复杂任务（比如小游戏）拿到真正需要的 display / lvgl / 输入模块文档。
class ModuleCatalog {
  ModuleCatalog._({
    required this.modules,
    required this.tierLabels,
    required this.implied,
    required this.perDocLimit,
    required this.totalLimit,
    required AssetBundle bundle,
    required Set<String> availableDocs,
  })  : _bundle = bundle,
        _availableDocs = availableDocs,
        byRequire = {for (final m in modules) m.require: m};

  final List<LuaModuleInfo> modules;
  final Map<String, String> tierLabels;
  final Map<String, List<String>> implied;
  final int perDocLimit;
  final int totalLimit;
  final Map<String, LuaModuleInfo> byRequire;

  final AssetBundle _bundle;

  /// manifest 里记录的、真实导出成功的组件。没有 README 的模块要跳过，
  /// 否则 loadAsset 会抛异常打断整次生成。
  final Set<String> _availableDocs;

  final Map<String, String> _docCache = {};

  static ModuleCatalog? _instance;

  /// 载入一次后缓存。catalog.json 只有几十 KB，但每轮重试都读一遍没有意义。
  static Future<ModuleCatalog> load({AssetBundle? bundle}) async {
    final existing = _instance;
    if (existing != null && bundle == null) return existing;

    final source = bundle ?? rootBundle;
    final catalogRaw = await source.loadString('assets/catalog.json');
    final manifestRaw = await source.loadString('assets/manifest.json');
    final catalogJson = jsonDecode(catalogRaw) as Map<String, dynamic>;
    final manifestJson = jsonDecode(manifestRaw) as Map<String, dynamic>;

    final modules = <LuaModuleInfo>[];
    for (final entry in (catalogJson['modules'] as List<dynamic>)) {
      final map = entry as Map<String, dynamic>;
      modules.add(LuaModuleInfo(
        require: map['require'] as String,
        component: map['component'] as String,
        tier: map['tier'] as String,
        summary: map['summary'] as String,
        keywords: ((map['keywords'] as List<dynamic>?) ?? const [])
            .map((e) => e as String)
            .toList(growable: false),
      ));
    }

    final catalog = ModuleCatalog._(
      modules: modules,
      tierLabels: (catalogJson['tier_labels'] as Map<String, dynamic>)
          .map((k, v) => MapEntry(k, v as String)),
      implied: (catalogJson['implied'] as Map<String, dynamic>).map(
        (k, v) => MapEntry(
            k, (v as List<dynamic>).map((e) => e as String).toList(growable: false)),
      ),
      perDocLimit: catalogJson['per_doc_limit'] as int,
      totalLimit: catalogJson['total_limit'] as int,
      bundle: source,
      availableDocs: ((manifestJson['module_docs'] as List<dynamic>?) ?? const [])
          .map((e) => e as String)
          .toSet(),
    );
    if (bundle == null) _instance = catalog;
    return catalog;
  }

  List<String> get knownRequires =>
      modules.map((m) => m.require).toList(growable: false);

  /// 按分类分组的紧凑清单，每个模块一行，用作规划阶段的上下文。
  String overview() {
    final lines = <String>[];
    tierLabels.forEach((tier, label) {
      final members = modules.where((m) => m.tier == tier).toList();
      if (members.isEmpty) return;
      lines.add('### $label');
      for (final item in members) {
        lines.add('- ${item.require}: ${item.summary}');
      }
    });
    return lines.join('\n');
  }

  /// 关键词兜底选择：LLM 规划失败时使用。
  List<String> selectByKeywords(String prompt) {
    final haystack = prompt.toLowerCase();
    final hits = <String>[];
    for (final item in modules) {
      if (item.keywords.any((kw) => haystack.contains(kw.toLowerCase()))) {
        hits.add(item.require);
      }
    }
    // 互斥规则：lvgl 和 display 不能同时使用，一律优先 lvgl。上层对象接口
    // （object/label/set_pos/set_style + on 事件）兼容性明显好于 display 的逐帧
    // 自绘，游戏和动画同样适用；lcd_touch 要保留，因为触摸是靠
    // board_manager.get_lcd_touch_handle 取到 handle 再 lvgl.indev_register 注册的。
    if (hits.contains('lvgl') && hits.contains('display')) {
      hits.remove('display');
    }
    return expandWithImplied(hits);
  }

  /// 补齐隐含依赖，保持调用方给出的顺序。
  List<String> expandWithImplied(List<String> requires) {
    final out = <String>[];
    for (final name in requires) {
      for (final dep in implied[name] ?? const <String>[]) {
        if (!out.contains(dep)) out.add(dep);
      }
      if (!out.contains(name)) out.add(name);
    }
    return out;
  }

  /// 把 LLM 返回的模块名清洗成目录里真实存在的 require 名。
  List<String> normalizeRequires(Object? raw) {
    if (raw is! List) return const [];
    final out = <String>[];
    for (final entry in raw) {
      if (entry is! String) continue;
      var name = entry.trim();
      name = _stripQuotes(name);
      if (name.startsWith('require(')) {
        name = name.substring('require('.length);
        if (name.endsWith(')')) name = name.substring(0, name.length - 1);
      }
      name = _stripQuotes(name.trim());
      if (byRequire.containsKey(name) && !out.contains(name)) out.add(name);
    }
    return out;
  }

  /// 把模块名映射为 (标签, 组件名)，共享同一 README 的模块合并成一条。
  List<({String label, String component})> resolveDocs(List<String> requires) {
    final ordered = <({String label, String component})>[];
    final seen = <String, int>{};
    for (final name in requires) {
      final info = byRequire[name];
      if (info == null) continue;
      if (!_availableDocs.contains(info.component)) continue;
      final index = seen[info.component];
      if (index != null) {
        ordered[index] = (
          label: '${ordered[index].label}, $name',
          component: info.component,
        );
        continue;
      }
      seen[info.component] = ordered.length;
      ordered.add((label: name, component: info.component));
    }
    return ordered;
  }

  /// 读取选中模块的 README，返回上下文文本和实际载入的资产路径。
  Future<({String context, List<String> docs})> loadModuleDocs(
    List<String> requires, {
    int? perDocLimit,
    int? totalLimit,
  }) async {
    final perDoc = perDocLimit ?? this.perDocLimit;
    final total = totalLimit ?? this.totalLimit;
    final sections = <String>[];
    final loaded = <String>[];
    var used = 0;

    for (final entry in resolveDocs(requires)) {
      if (used >= total) break;
      final asset = 'assets/lua_modules/${entry.component}.md';
      var text = _docCache[asset] ?? '';
      if (text.isEmpty) {
        try {
          text = (await _bundle.loadString(asset)).trim();
        } on Object {
          // 资产缺失或读不出来就跳过这一份，不该让整次生成失败。
          continue;
        }
        _docCache[asset] = text;
      }
      final budget = perDoc < total - used ? perDoc : total - used;
      if (text.length > budget) {
        text = '${text.substring(0, budget).trimRight()}'
            '\n...[README 截断，未显示的部分请勿凭猜测使用]';
      }
      sections.add('## require("${entry.label}") -> ${entry.component}\n$text');
      loaded.add('components/lua_modules/${entry.component}/README.md');
      used += text.length;
    }

    return (context: sections.join('\n\n'), docs: loaded);
  }

  static String _stripQuotes(String value) {
    var out = value;
    while (out.isNotEmpty &&
        (out.startsWith('"') || out.startsWith("'"))) {
      out = out.substring(1);
    }
    while (out.isNotEmpty && (out.endsWith('"') || out.endsWith("'"))) {
      out = out.substring(0, out.length - 1);
    }
    return out;
  }
}
