// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'package:flutter/material.dart';

import '../app_state.dart';

/// 设备页：扫描、连接、断开、取设备信息。
class DevicePage extends StatelessWidget {
  const DevicePage({super.key});

  @override
  Widget build(BuildContext context) {
    final state = AppStateScope.of(context);

    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            FilledButton.icon(
              onPressed: state.scanning ? null : state.scan,
              icon: state.scanning
                  ? const SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.bluetooth_searching),
              label: Text(state.scanning ? '扫描中…' : '扫描设备'),
            ),
            if (state.connected) ...[
              OutlinedButton.icon(
                onPressed: state.refreshDeviceInfo,
                icon: const Icon(Icons.refresh),
                label: const Text('刷新设备信息'),
              ),
              OutlinedButton.icon(
                onPressed: state.disconnect,
                icon: const Icon(Icons.link_off),
                label: const Text('断开'),
              ),
            ],
          ],
        ),
        const SizedBox(height: 16),
        if (state.devices.isEmpty && !state.scanning)
          const Text(
            '按 service UUID 扫描，所以设备广播名拿不到也能扫到。'
            '扫不到时确认设备已开机、BLE bridge 已启用，且没有被其它手机占着连接。',
            style: TextStyle(fontSize: 12),
          ),
        ...state.devices.map((device) {
          final isCurrent = state.connected && state.deviceLabel == device.name;
          return Card(
            margin: const EdgeInsets.only(bottom: 8),
            child: ListTile(
              leading: Icon(
                isCurrent ? Icons.bluetooth_connected : Icons.bluetooth,
                color: isCurrent ? Theme.of(context).colorScheme.primary : null,
              ),
              title: Text(device.name),
              subtitle: Text('${device.id}   RSSI ${device.rssi}'),
              trailing: isCurrent
                  ? const Text('已连接')
                  : TextButton(
                      onPressed:
                          state.connecting ? null : () => state.connect(device),
                      child: const Text('连接'),
                    ),
            ),
          );
        }),
        if (state.deviceInfo.isNotEmpty) ...[
          const Divider(height: 32),
          Text('设备信息', style: Theme.of(context).textTheme.titleSmall),
          const SizedBox(height: 8),
          const Text(
            '这段会作为生成阶段的上下文交给 LLM。',
            style: TextStyle(fontSize: 12),
          ),
          const SizedBox(height: 8),
          SelectableText(
            state.deviceInfo,
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
          ),
        ],
      ],
    );
  }
}
