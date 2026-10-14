// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'package:flutter/material.dart';

import 'src/app_state.dart';
import 'src/ui/home_page.dart';

void main() {
  runApp(const VibeApp());
}

class VibeApp extends StatefulWidget {
  const VibeApp({super.key});

  @override
  State<VibeApp> createState() => _VibeAppState();
}

class _VibeAppState extends State<VibeApp> {
  final AppState _state = AppState();

  @override
  void initState() {
    super.initState();
    _state.bootstrap();
  }

  @override
  void dispose() {
    _state.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESP-Claw Vibe',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFFE7352B)),
        useMaterial3: true,
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFFE7352B),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: AppStateScope(state: _state, child: const HomePage()),
    );
  }
}
