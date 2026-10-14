// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'dart:async';
import 'dart:convert';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'constants.dart';

/// 请求/响应的收发抽象。单测里换成假实现就能覆盖迭代循环，不需要真设备。
abstract class Bridge {
  Future<Map<String, dynamic>> request(
    Map<String, dynamic> command, {
    Duration? timeout,
  });
}

int _requestCounter = 0;

String requestId(String prefix) {
  _requestCounter++;
  final stamp = DateTime.now().microsecondsSinceEpoch.toRadixString(36);
  return '$prefix-$stamp-$_requestCounter';
}

/// 按 UTF-8 字节数切分，不切断多字节字符。
List<String> splitByUtf8Size(String text, int maxBytes) {
  final chunks = <String>[];
  final current = StringBuffer();
  var currentSize = 0;
  for (final rune in text.runes) {
    final char = String.fromCharCode(rune);
    final size = utf8.encode(char).length;
    if (currentSize > 0 && currentSize + size > maxBytes) {
      chunks.add(current.toString());
      current.clear();
      currentSize = 0;
    }
    current.write(char);
    currentSize += size;
  }
  if (current.isNotEmpty) chunks.add(current.toString());
  return chunks;
}

class BleException implements Exception {
  const BleException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// 扫描到的一个候选设备。
class ScannedDevice {
  const ScannedDevice({required this.device, required this.name, required this.rssi});

  final BluetoothDevice device;
  final String name;
  final int rssi;

  String get id => device.remoteId.str;
}

/// ESP-Claw BLE bridge 客户端。
///
/// 协议与电脑端一致：请求写 RX，响应从 TX 通知回来，超过单帧的按 seq 分片。
/// 设备端 ble_host_bridge.c 的约束决定了两件事：
/// - 请求队列只有 6 格，且分片必须严格按序到达，所以写入要串行、带 response；
/// - 响应分片按 (id, seq, total) 重组，收齐才是一帧完整 JSON。
class BleBridge implements Bridge {
  BleBridge({
    this.namePrefix = defaultDevicePrefix,
    this.timeout = defaultTimeout,
    this.onDeviceEvent,
    this.onLog,
  });

  final String namePrefix;
  final Duration timeout;

  /// 设备端 job 结束时会主动推一帧，这里收下来，避免被当成无主响应丢掉。
  final void Function(Map<String, dynamic> frame)? onDeviceEvent;
  final void Function(String message)? onLog;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _rx;
  BluetoothCharacteristic? _tx;
  StreamSubscription<List<int>>? _notifySub;
  StreamSubscription<BluetoothConnectionState>? _stateSub;

  final Map<String, Completer<Map<String, dynamic>>> _pending = {};
  final Map<String, ({int total, Map<int, String> parts})> _responseChunks = {};

  /// 写入必须串行：设备端分片重组要求严格按序，并发写会打乱 seq。
  Future<void> _writeChain = Future.value();

  Map<String, dynamic>? lastDeviceEvent;

  bool get isConnected => _device?.isConnected ?? false;

  String get deviceLabel {
    final device = _device;
    if (device == null) return '';
    final name = device.platformName;
    return name.isEmpty ? device.remoteId.str : name;
  }

  /// 扫描带 bridge service UUID 的设备。
  ///
  /// 按 service UUID 过滤而不是只看名字：Android 上广播名可能拿不到，
  /// 但 service UUID 一定在广播包里。
  static Future<List<ScannedDevice>> scan({
    Duration duration = const Duration(seconds: 6),
    String namePrefix = defaultDevicePrefix,
  }) async {
    final found = <String, ScannedDevice>{};
    final sub = FlutterBluePlus.scanResults.listen((results) {
      for (final result in results) {
        final name = result.device.platformName.isNotEmpty
            ? result.device.platformName
            : result.advertisementData.advName;
        found[result.device.remoteId.str] = ScannedDevice(
          device: result.device,
          name: name.isEmpty ? '(未命名)' : name,
          rssi: result.rssi,
        );
      }
    });

    try {
      await FlutterBluePlus.startScan(
        withServices: [Guid(serviceUuid)],
        timeout: duration,
      );
      await FlutterBluePlus.isScanning.where((on) => !on).first;
    } finally {
      await sub.cancel();
      await FlutterBluePlus.stopScan();
    }

    final list = found.values.toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
    // 名字匹配的排前面，但不过滤掉其它——按 service UUID 扫到的都是候选。
    list.sort((a, b) {
      final aMatch = a.name.toLowerCase().startsWith(namePrefix) ? 0 : 1;
      final bMatch = b.name.toLowerCase().startsWith(namePrefix) ? 0 : 1;
      return aMatch.compareTo(bMatch);
    });
    return list;
  }

  Future<void> connect(BluetoothDevice device) async {
    await disconnect();
    _device = device;

    await device.connect(timeout: timeout, autoConnect: false);

    // 掉线要立刻把所有等待者叫醒，否则调用方会一直卡到自己的超时。
    _stateSub = device.connectionState.listen((state) {
      if (state == BluetoothConnectionState.disconnected) {
        _failAllPending(const BleException('BLE 连接已断开'));
      }
    });

    // MTU 越大分片越少。Android 上这是请求值，设备端 preferred 是 247。
    try {
      await device.requestMtu(preferredMtu);
    } on Object {
      // 部分机型不允许应用侧请求 MTU，用协商结果继续即可。
    }

    final services = await device.discoverServices();
    BluetoothService? bridgeService;
    for (final service in services) {
      if (service.uuid == Guid(serviceUuid)) {
        bridgeService = service;
        break;
      }
    }
    if (bridgeService == null) {
      await disconnect();
      throw const BleException('设备上找不到 ESP-Claw bridge service，可能不是目标设备');
    }

    for (final char in bridgeService.characteristics) {
      if (char.uuid == Guid(rxUuid)) _rx = char;
      if (char.uuid == Guid(txUuid)) _tx = char;
    }
    if (_rx == null || _tx == null) {
      await disconnect();
      throw const BleException('bridge service 缺少 RX/TX characteristic');
    }

    _notifySub = _tx!.onValueReceived.listen(_onNotify);
    await _tx!.setNotifyValue(true);
    onLog?.call('已连接 $deviceLabel');
  }

  Future<void> disconnect() async {
    await _notifySub?.cancel();
    _notifySub = null;
    await _stateSub?.cancel();
    _stateSub = null;
    final device = _device;
    if (device != null && device.isConnected) {
      try {
        await device.disconnect();
      } on Object {
        // 已经断了就算了。
      }
    }
    _failAllPending(const BleException('BLE 连接已关闭'));
    _responseChunks.clear();
    _rx = null;
    _tx = null;
    _device = null;
  }

  void _failAllPending(Object error) {
    final pending = Map.of(_pending);
    _pending.clear();
    for (final completer in pending.values) {
      if (!completer.isCompleted) completer.completeError(error);
    }
  }

  void _onNotify(List<int> data) {
    final text = utf8.decode(data, allowMalformed: true);
    Map<String, dynamic> frame;
    try {
      final decoded = jsonDecode(text);
      frame = decoded is Map<String, dynamic>
          ? decoded
          : {'id': '', 'ok': true, 'type': 'raw', 'data': text};
    } on FormatException {
      frame = {'id': '', 'ok': true, 'type': 'raw', 'data': text};
    }

    final kind = '${frame['kind'] ?? frame['k'] ?? ''}';
    if (kind == 'response_chunk' || kind == 'c') {
      final reqId = '${frame['id'] ?? frame['i'] ?? ''}';
      final total = _asInt(frame['total'] ?? frame['t']);
      final seq = _asInt(frame['seq'] ?? frame['s']);
      final payload = '${frame['data'] ?? frame['d'] ?? ''}';

      final current =
          _responseChunks.putIfAbsent(reqId, () => (total: total, parts: {}));
      final effectiveTotal = total > 0 ? total : current.total;
      current.parts[seq] = payload;
      _responseChunks[reqId] = (total: effectiveTotal, parts: current.parts);

      if (effectiveTotal > 0 && current.parts.length == effectiveTotal) {
        final joined = StringBuffer();
        for (var i = 0; i < effectiveTotal; i++) {
          joined.write(current.parts[i] ?? '');
        }
        _responseChunks.remove(reqId);
        try {
          final decoded = jsonDecode(joined.toString());
          if (decoded is Map<String, dynamic>) {
            _resolvePending(reqId, decoded);
            return;
          }
        } on FormatException {
          _resolvePending(reqId, {
            'id': reqId,
            'ok': false,
            'error': '分片重组后不是合法 JSON',
          });
          return;
        }
      }
      return;
    }

    _resolvePending('${frame['id'] ?? ''}', frame);
  }

  void _resolvePending(String reqId, Map<String, dynamic> frame) {
    final completer = _pending.remove(reqId);
    if (completer != null && !completer.isCompleted) {
      completer.complete(frame);
      return;
    }
    // 没有等待者：可能是设备主动推的 job 结果。
    if (reqId == deviceEventRequestId || frame['type'] == 'job_event') {
      lastDeviceEvent = frame;
      onDeviceEvent?.call(frame);
    }
  }

  @override
  Future<Map<String, dynamic>> request(
    Map<String, dynamic> command, {
    Duration? timeout,
  }) async {
    if (!isConnected || _rx == null) {
      throw const BleException('BLE bridge 未连接');
    }
    final reqId = '${command['id'] ?? requestId('req')}';
    final payload = {...command, 'id': reqId};
    final completer = Completer<Map<String, dynamic>>();
    _pending[reqId] = completer;

    try {
      await _send(payload);
      return await completer.future.timeout(timeout ?? this.timeout);
    } on Object {
      _pending.remove(reqId);
      _responseChunks.remove(reqId);
      rethrow;
    }
  }

  Future<void> _send(Map<String, dynamic> command) {
    // 串到写队列末尾：设备端要求分片按序，并发写会让 seq 乱掉。
    final task = _writeChain.then((_) => _sendNow(command));
    _writeChain = task.catchError((_) {});
    return task;
  }

  Future<void> _sendNow(Map<String, dynamic> command) async {
    final rx = _rx;
    if (rx == null) throw const BleException('BLE bridge 未连接');

    final text = jsonEncode(command);
    final bytes = utf8.encode(text);
    if (bytes.length <= requestChunkSize) {
      // withoutResponse=false：设备端请求队列只有 6 格，带响应写能拿到背压，
      // 不至于把队列冲满导致整帧被丢。
      await rx.write(bytes, withoutResponse: false);
      return;
    }

    final chunks = splitByUtf8Size(text, requestChunkSize);
    for (var seq = 0; seq < chunks.length; seq++) {
      final frame = {
        'i': command['id'],
        'k': 'c',
        'o': command['op'] ?? '',
        's': seq,
        't': chunks.length,
        'd': chunks[seq],
      };
      await rx.write(utf8.encode(jsonEncode(frame)), withoutResponse: false);
    }
  }

  static int _asInt(Object? value) {
    if (value is int) return value;
    if (value is num) return value.toInt();
    return int.tryParse('$value') ?? 0;
  }
}
