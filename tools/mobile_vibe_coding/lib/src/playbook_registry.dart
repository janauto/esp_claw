// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:convert';

import 'package:flutter/services.dart';

import 'constants.dart';

final RegExp _frontmatterRe = RegExp(r'^---\s*\n([\s\S]*?)\n---\s*\n');

/// frontmatter 里的 match 条件。任一维度为空表示该维度不限制；
/// 同一维度内是 OR，不同维度之间是 AND。
class PlaybookMatch {
  const PlaybookMatch({
    this.taskKind = const [],
    this.uiStack = const [],
    this.modules = const [],
  });

  final List<String> taskKind;
  final List<String> uiStack;
  final List<String> modules;

  /// 返回匹配得分；0 表示不匹配。得分高的优先，用于挑最贴合的 playbook。
  int score(String taskKind, String uiStack, List<String> modules) {
    var points = 0;
    if (this.taskKind.isNotEmpty) {
      if (!this.taskKind.contains(taskKind)) return 0;
      points += 4;
    }
    if (this.uiStack.isNotEmpty) {
      if (!this.uiStack.contains(uiStack)) return 0;
      points += 3;
    }
    if (this.modules.isNotEmpty) {
      final hit = this.modules.where(modules.contains).length;
      if (hit == 0) return 0;
      points += hit;
    }
    return points == 0 ? 1 : points;
  }
}

/// 一份任务类型专属的实现规范。
class Playbook {
  const Playbook({
    required this.name,
    required this.description,
    required this.asset,
    required this.match,
    required this.priority,
    required this.always,
    required String rawBody,
  }) : _body = rawBody;

  final String name;
  final String description;
  final String asset;
  final PlaybookMatch match;
  final int priority;
  final bool always;
  final String _body;

  String body() {
    final text = _body.trim();
    if (text.length <= maxPlaybookChars) return text;
    return '${text.substring(0, maxPlaybookChars).trimRight()}\n...[playbook 截断]';
  }
}

/// 扫描 assets 建立 playbook 注册表。对标 host_vibe_coding/playbook_registry.py。
class PlaybookRegistry {
  const PlaybookRegistry(this.playbooks);

  final List<Playbook> playbooks;

  static PlaybookRegistry? _instance;

  static Future<PlaybookRegistry> load({AssetBundle? bundle}) async {
    final existing = _instance;
    if (existing != null && bundle == null) return existing;

    final source = bundle ?? rootBundle;
    final manifestRaw = await source.loadString('assets/manifest.json');
    final manifest = jsonDecode(manifestRaw) as Map<String, dynamic>;
    final names = ((manifest['playbooks'] as List<dynamic>?) ?? const [])
        .map((e) => e as String)
        .toList()
      ..sort();

    final out = <Playbook>[];
    for (final fileName in names) {
      final asset = 'assets/playbooks/$fileName';
      String raw;
      try {
        raw = await source.loadString(asset);
      } on Object {
        // manifest 里列了但资产没打进包：跳过这一份，别让整个注册表加载失败。
        continue;
      }
      final parsed = _parse(raw, asset, fileName);
      if (parsed != null) out.add(parsed);
    }

    final registry = PlaybookRegistry(out);
    if (bundle == null) _instance = registry;
    return registry;
  }

  /// 解析 JSON frontmatter。用 JSON 而不是 YAML：dart:convert 就能解析，
  /// 不给手机端引入额外依赖。
  static Playbook? _parse(String raw, String asset, String fileName) {
    final found = _frontmatterRe.firstMatch(raw);
    Map<String, dynamic> meta = const {};
    var body = raw;
    if (found != null) {
      try {
        final decoded = jsonDecode(found.group(1)!);
        if (decoded is Map<String, dynamic>) {
          meta = decoded;
          body = raw.substring(found.end);
        }
      } on FormatException {
        // frontmatter 坏了就当整篇是正文，名字退回文件名。
      }
    }

    final stem = fileName.endsWith('.md')
        ? fileName.substring(0, fileName.length - 3)
        : fileName;
    final name = ((meta['name'] as String?) ?? stem).trim();
    if (name.isEmpty || body.trim().isEmpty) return null;

    final matchRaw =
        meta['match'] is Map<String, dynamic> ? meta['match'] as Map<String, dynamic> : const {};
    final rawPriority = meta['priority'];
    final priority = rawPriority is int
        ? rawPriority
        : int.tryParse('$rawPriority') ?? 50;

    return Playbook(
      name: name,
      description: ((meta['description'] as String?) ?? '').trim(),
      asset: asset,
      match: PlaybookMatch(
        taskKind: _asList(matchRaw['task_kind']),
        uiStack: _asList(matchRaw['ui_stack']),
        modules: _asList(matchRaw['modules']),
      ),
      priority: priority,
      always: meta['always'] == true,
      rawBody: body,
    );
  }

  static List<String> _asList(Object? raw) {
    if (raw is String) {
      final value = raw.trim().toLowerCase();
      return value.isEmpty ? const [] : [value];
    }
    if (raw is List) {
      return raw
          .whereType<String>()
          .map((e) => e.trim().toLowerCase())
          .where((e) => e.isNotEmpty)
          .toList(growable: false);
    }
    return const [];
  }

  String overview() => playbooks
      .where((pb) => pb.description.isNotEmpty)
      .map((pb) => '- ${pb.name}: ${pb.description}')
      .join('\n');

  /// 挑出适用的 playbook，按匹配度排序。always=true 的无条件入选。
  List<Playbook> select({
    required String taskKind,
    required String uiStack,
    required List<String> modules,
    int limit = 3,
  }) {
    final always = playbooks.where((pb) => pb.always).toList();
    final scored = <({int score, int priority, Playbook pb})>[];
    for (final pb in playbooks) {
      if (pb.always) continue;
      final score = pb.match.score(taskKind, uiStack, modules);
      if (score > 0) {
        scored.add((score: score, priority: pb.priority, pb: pb));
      }
    }
    scored.sort((a, b) {
      final byScore = b.score.compareTo(a.score);
      return byScore != 0 ? byScore : a.priority.compareTo(b.priority);
    });
    return [
      ...always,
      ...scored.take(limit).map((e) => e.pb),
    ];
  }

  /// 把选中的 playbook 正文拼成一段上下文。
  static String render(List<Playbook> picked) => picked
      .map((pb) => (name: pb.name, body: pb.body()))
      .where((e) => e.body.isNotEmpty)
      .map((e) => '### ${e.name}\n\n${e.body}')
      .join('\n\n');
}
