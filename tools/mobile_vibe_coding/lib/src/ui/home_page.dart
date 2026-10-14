// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';
import '../example_scripts.dart';
import 'device_page.dart';
import 'settings_page.dart';

/// 主界面：需求输入 -> 生成 -> 编辑 -> 执行 -> 看日志，四个标签页。
class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with SingleTickerProviderStateMixin {
  late final TabController _tabs = TabController(length: 4, vsync: this);

  @override
  void dispose() {
    _tabs.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final state = AppStateScope.of(context);

    if (state.bootError.isNotEmpty) {
      return Scaffold(
        appBar: AppBar(title: const Text('ESP-Claw Vibe')),
        body: Center(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Text('启动失败：${state.bootError}'),
          ),
        ),
      );
    }
    if (!state.ready) {
      return const Scaffold(body: Center(child: CircularProgressIndicator()));
    }

    return Scaffold(
      appBar: AppBar(
        title: const Text('ESP-Claw Vibe'),
        actions: [
          IconButton(
            tooltip: 'LLM 设置',
            icon: const Icon(Icons.settings_outlined),
            onPressed: () => Navigator.of(context).push(
              MaterialPageRoute<void>(
                builder: (_) => AppStateScope(
                  state: state,
                  child: const SettingsPage(),
                ),
              ),
            ),
          ),
        ],
        bottom: TabBar(
          controller: _tabs,
          isScrollable: true,
          tabAlignment: TabAlignment.start,
          tabs: const [
            Tab(text: '设备'),
            Tab(text: '需求'),
            Tab(text: 'Lua'),
            Tab(text: '日志'),
          ],
        ),
      ),
      body: Column(
        children: [
          _StatusBar(state: state),
          Expanded(
            child: TabBarView(
              controller: _tabs,
              children: [
                const DevicePage(),
                _PromptTab(state: state),
                _ScriptTab(state: state),
                _LogTab(state: state),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _StatusBar extends StatelessWidget {
  const _StatusBar({required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final connected = state.connected;
    return Material(
      color: theme.colorScheme.surfaceContainerHighest,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
        child: Row(
          children: [
            Icon(
              connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
              size: 18,
              color: connected ? theme.colorScheme.primary : theme.disabledColor,
            ),
            const SizedBox(width: 6),
            Expanded(
              child: Text(
                connected ? state.deviceLabel : '未连接设备',
                style: theme.textTheme.bodySmall,
                overflow: TextOverflow.ellipsis,
              ),
            ),
            if (state.busy) ...[
              const SizedBox(
                width: 14,
                height: 14,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
              const SizedBox(width: 6),
              Text(state.busyLabel, style: theme.textTheme.bodySmall),
            ] else
              Text(state.config.model, style: theme.textTheme.bodySmall),
          ],
        ),
      ),
    );
  }
}

class _PromptTab extends StatelessWidget {
  const _PromptTab({required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    final busy = state.busy;
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        TextField(
          controller: state.promptController,
          minLines: 4,
          maxLines: 8,
          decoration: const InputDecoration(
            labelText: '想让设备做什么',
            hintText: '例如：做一个贪吃蛇小游戏，用触摸控制',
            border: OutlineInputBorder(),
            alignLabelWithHint: true,
          ),
        ),
        const SizedBox(height: 12),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            OutlinedButton.icon(
              onPressed: busy ? null : state.previewPlan,
              icon: const Icon(Icons.travel_explore_outlined),
              label: const Text('查看模块选择'),
            ),
            FilledButton.icon(
              onPressed: busy ? null : state.generate,
              icon: const Icon(Icons.auto_awesome_outlined),
              label: const Text('生成 Lua'),
            ),
            FilledButton.tonalIcon(
              onPressed: busy || !state.connected ? null : state.generateAndIterate,
              icon: const Icon(Icons.loop),
              label: const Text('生成并自动迭代'),
            ),
          ],
        ),
        const SizedBox(height: 8),
        if (!state.connected)
          const Text(
            '「查看模块选择」和「生成 Lua」不需要连接设备；自动迭代要靠设备回传的真实报错，必须先连上。',
            style: TextStyle(fontSize: 12),
          ),
        const Divider(height: 32),
        Text('迭代参数', style: Theme.of(context).textTheme.titleSmall),
        const SizedBox(height: 8),
        _StepperRow(
          label: '最多轮数',
          value: state.maxAttempts,
          onChanged: state.setMaxAttempts,
          step: 1,
          suffix: '轮',
        ),
        _StepperRow(
          label: '等待设备',
          value: state.pollTimeoutS,
          onChanged: state.setPollTimeout,
          step: 15,
          suffix: '秒',
        ),
        const SizedBox(height: 8),
        const Text(
          '常驻类脚本（游戏、界面）会一直停在 running，等待时间就是「观察多久算跑稳了」。'
          '真正的 Lua 报错才会消耗下一轮。',
          style: TextStyle(fontSize: 12),
        ),
        if (state.lastPlan != null) ...[
          const Divider(height: 32),
          Text('最近一次规划', style: Theme.of(context).textTheme.titleSmall),
          const SizedBox(height: 8),
          SelectableText(
            state.lastPlan!.describe(),
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
          ),
        ],
      ],
    );
  }
}

class _StepperRow extends StatelessWidget {
  const _StepperRow({
    required this.label,
    required this.value,
    required this.onChanged,
    required this.step,
    required this.suffix,
  });

  final String label;
  final int value;
  final void Function(int) onChanged;
  final int step;
  final String suffix;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(child: Text(label)),
        IconButton(
          icon: const Icon(Icons.remove_circle_outline),
          onPressed: () => onChanged(value - step),
        ),
        Text('$value $suffix'),
        IconButton(
          icon: const Icon(Icons.add_circle_outline),
          onPressed: () => onChanged(value + step),
        ),
      ],
    );
  }
}

class _ScriptTab extends StatelessWidget {
  const _ScriptTab({required this.state});

  final AppState state;

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Expanded(
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: TextField(
              controller: state.scriptController,
              expands: true,
              maxLines: null,
              minLines: null,
              keyboardType: TextInputType.multiline,
              style: const TextStyle(fontFamily: 'monospace', fontSize: 13),
              decoration: const InputDecoration(
                border: OutlineInputBorder(),
                hintText: '生成的 Lua 会落在这里，可以直接改完再上传。',
                alignLabelWithHint: true,
              ),
            ),
          ),
        ),
        SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            child: Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                FilledButton.icon(
                  onPressed: state.busy || !state.connected
                      ? null
                      : state.runCurrentScript,
                  icon: const Icon(Icons.play_arrow),
                  label: const Text('上传并执行'),
                ),
                OutlinedButton.icon(
                  onPressed: state.busy
                      ? null
                      : () => _showExamplePicker(context, state),
                  icon: const Icon(Icons.folder_open_outlined),
                  label: const Text('打开示例'),
                ),
                OutlinedButton.icon(
                  onPressed: state.busy || !state.connected
                      ? null
                      : state.stopJob,
                  icon: const Icon(Icons.stop_outlined),
                  label: const Text('停止 Job'),
                ),
                OutlinedButton.icon(
                  onPressed: () async {
                    await Clipboard.setData(
                        ClipboardData(text: state.scriptController.text));
                    state.log('脚本已复制到剪贴板');
                  },
                  icon: const Icon(Icons.copy_outlined),
                  label: const Text('复制'),
                ),
                OutlinedButton.icon(
                  onPressed: () async {
                    final data = await Clipboard.getData(Clipboard.kTextPlain);
                    final text = data?.text ?? '';
                    if (text.isEmpty) {
                      state.log('剪贴板里没有文本');
                      return;
                    }
                    state.scriptController.text = text;
                    state.log('已从剪贴板粘贴 ${text.length} 字符');
                  },
                  icon: const Icon(Icons.paste_outlined),
                  label: const Text('粘贴'),
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

class _LogTab extends StatefulWidget {
  const _LogTab({required this.state});

  final AppState state;

  @override
  State<_LogTab> createState() => _LogTabState();
}

class _LogTabState extends State<_LogTab> {
  final ScrollController _scroll = ScrollController();
  int _lastCount = 0;

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  void _autoScroll() {
    // 只在有新行时跟到底部，避免用户往上翻查日志时被拽回来。
    if (widget.state.logLines.length == _lastCount) return;
    _lastCount = widget.state.logLines.length;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) {
        _scroll.jumpTo(_scroll.position.maxScrollExtent);
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final state = widget.state;
    _autoScroll();
    return Column(
      children: [
        Expanded(
          child: state.logLines.isEmpty
              ? const Center(child: Text('还没有日志'))
              : ListView.builder(
                  controller: _scroll,
                  padding: const EdgeInsets.all(12),
                  itemCount: state.logLines.length,
                  itemBuilder: (context, index) => Padding(
                    padding: const EdgeInsets.only(bottom: 4),
                    child: SelectableText(
                      state.logLines[index],
                      style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
                    ),
                  ),
                ),
        ),
        SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            child: Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                OutlinedButton.icon(
                  onPressed: state.connected ? state.queryJob : null,
                  icon: const Icon(Icons.help_outline),
                  label: const Text('查询 job'),
                ),
                OutlinedButton.icon(
                  onPressed: state.connected ? state.fetchFullLog : null,
                  icon: const Icon(Icons.notes_outlined),
                  label: const Text('完整日志'),
                ),
                OutlinedButton.icon(
                  onPressed: state.connected ? state.stopJob : null,
                  icon: const Icon(Icons.stop_circle_outlined),
                  label: const Text('停止 job'),
                ),
                TextButton.icon(
                  onPressed: state.clearLog,
                  icon: const Icon(Icons.clear_all),
                  label: const Text('清空'),
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

/// 显示示例脚本选择对话框
Future<void> _showExamplePicker(BuildContext context, AppState state) async {
  final selected = await showDialog<ExampleScript>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('选择示例脚本'),
      content: SizedBox(
        width: double.maxFinite,
        child: ListView.builder(
          shrinkWrap: true,
          itemCount: exampleScripts.length,
          itemBuilder: (context, index) {
            final example = exampleScripts[index];
            return ListTile(
              leading: const Icon(Icons.code),
              title: Text(example.name),
              subtitle: Text(example.description),
              onTap: () => Navigator.of(context).pop(example),
            );
          },
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
      ],
    ),
  );

  if (selected != null) {
    await state.loadExampleFromAssets(selected.assetPath, selected.name);
  }
}

