// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'package:flutter/services.dart';

/// 示例 Lua 脚本元数据
class ExampleScript {
  const ExampleScript({
    required this.name,
    required this.assetPath,
    this.description = '',
  });

  final String name;
  final String assetPath;
  final String description;
}

/// 可用的示例脚本列表
const List<ExampleScript> exampleScripts = [
  ExampleScript(
    name: '贪吃蛇',
    assetPath: 'assets/examples/贪吃蛇.lua',
    description: '经典贪吃蛇游戏，触摸控制方向',
  ),
  ExampleScript(
    name: '跳跃游戏',
    assetPath: 'assets/examples/跳跃游戏.lua',
    description: '横版跳跃游戏，点击屏幕跳跃',
  ),
  ExampleScript(
    name: '消消乐',
    assetPath: 'assets/examples/消消乐.lua',
    description: '三消游戏，触摸交换方块',
  ),
];

/// 加载示例脚本内容
Future<String> loadExampleScript(String assetPath) async {
  try {
    return await rootBundle.loadString(assetPath);
  } on Object catch (err) {
    throw Exception('加载示例脚本失败：$err');
  }
}
