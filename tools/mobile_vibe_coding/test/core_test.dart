// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import 'package:esp_claw_vibe/src/ble_bridge.dart';
import 'package:esp_claw_vibe/src/job_outcome.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('parseJobStatus', () {
    test('切出带换行的 recent_log', () {
      const text = 'job_id=abc123\n'
          'status=failed\n'
          'runtime_s=2\n'
          'summary=attempt to call a nil value\n'
          'log_truncated=true\n'
          'recent_log=line1\nline2\nline3';
      final outcome = parseJobStatus(text);
      expect(outcome.jobId, 'abc123');
      expect(outcome.status, 'failed');
      expect(outcome.runtimeS, 2);
      expect(outcome.logTruncated, isTrue);
      expect(outcome.log, 'line1\nline2\nline3');
    });

    test('缺字段时退回 unknown 而不是抛异常', () {
      final outcome = parseJobStatus('garbage');
      expect(outcome.status, 'unknown');
      expect(outcome.runtimeS, 0);
    });
  });

  group('JobOutcome 成败方向', () {
    test('一次性脚本只认 done', () {
      const done = JobOutcome(jobId: 'a', status: 'done');
      const failed = JobOutcome(jobId: 'a', status: 'failed');
      expect(done.ok, isTrue);
      expect(failed.ok, isFalse);
      expect(failed.retryable, isTrue);
    });

    test('常驻脚本 running 够久算成功，且不再重试', () {
      const running = JobOutcome(
          jobId: 'a', status: 'running', runtimeS: 30, expectsLongRun: true);
      expect(running.ok, isTrue);
      expect(running.retryable, isFalse);
    });

    test('常驻脚本 timeout 是成功信号，不消耗下一轮', () {
      const timeout =
          JobOutcome(jobId: 'a', status: 'timeout', expectsLongRun: true);
      expect(timeout.ok, isTrue);
      expect(timeout.retryable, isFalse);
    });

    test('常驻脚本 done 且秒退是失败，值得重试', () {
      const flicker = JobOutcome(
          jobId: 'a', status: 'done', runtimeS: 1, expectsLongRun: true);
      expect(flicker.ok, isFalse);
      expect(flicker.flickered, isTrue);
      expect(flicker.retryable, isTrue);
      // 设备不报错，诊断由上位机合成，所以必须自认有诊断信息。
      expect(flicker.hasDiagnostics, isTrue);
      expect(flicker.describe(), contains('主循环'));
    });

    test('stopped 是有人主动停的，不重试', () {
      const stopped = JobOutcome(jobId: 'a', status: 'stopped');
      expect(stopped.retryable, isFalse);
    });

    test('占位文本不算诊断信息', () {
      const empty =
          JobOutcome(jobId: 'a', status: 'failed', summary: '(empty)', log: '(none)');
      expect(empty.hasDiagnostics, isFalse);
    });

    test('错误签名抹掉 job id，同一个错误只算一种', () {
      const first = JobOutcome(
          jobId: 'a', status: 'failed', log: '/scripts/host/deadbeef01.lua: error x');
      const second = JobOutcome(
          jobId: 'b', status: 'failed', log: '/scripts/host/cafe12345f.lua: error x');
      expect(first.errorSignature(), second.errorSignature());
    });
  });

  group('extractLua', () {
    test('取最像完整程序的块，而不是第一个片段', () {
      const reply = '先看个片段：\n'
          '```lua\nlocal x = 1\n```\n'
          '完整实现：\n'
          '```lua\nlocal lvgl = require("lvgl")\nwhile true do end\n```';
      final script = extractLua(reply);
      expect(script, contains('require("lvgl")'));
      expect(script, isNot(contains('local x = 1')));
    });

    test('收尾 fence 缺失时不把反引号带给设备', () {
      const truncated = '```lua\nlocal m = require("system")\nprint(m)';
      final script = extractLua(truncated);
      expect(script, isNot(contains('```')));
      expect(script, contains('require("system")'));
    });
  });

  group('validateGeneratedLua', () {
    test('残留围栏判失败，省一次设备往返', () {
      expect(
        () => validateGeneratedLua('local a = require("system")\n```'),
        throwsA(isA<LuaValidationException>()),
      );
    });

    test('没有 require 的片段判失败', () {
      expect(
        () => validateGeneratedLua('local a = 1'),
        throwsA(isA<LuaValidationException>()),
      );
    });

    test('建了 LVGL 屏幕却没有主循环判失败', () {
      const script = 'local lvgl = require("lvgl")\n'
          'local scr = lvgl.create_screen()\n';
      expect(
        () => validateGeneratedLua(script),
        throwsA(isA<LuaValidationException>()),
      );
    });

    test('带主循环的界面脚本通过', () {
      const script = 'local lvgl = require("lvgl")\n'
          'local scr = lvgl.create_screen()\n'
          'while true do lvgl.process_events(16) end\n';
      expect(() => validateGeneratedLua(script), returnsNormally);
    });
  });

  group('scriptExpectsLongRun', () {
    test('只写主循环、复用已有屏幕也算常驻', () {
      expect(
        scriptExpectsLongRun('while true do lvgl.process_events(16) end'),
        isTrue,
      );
    });

    test('纯采集脚本不算常驻', () {
      expect(scriptExpectsLongRun('local s = require("system")\nprint(s.heap())'),
          isFalse);
    });
  });

  group('normalizeScript', () {
    test('忽略注释与空白差异', () {
      const a = 'local x = 1 -- 注释\n\n  print(x)\n';
      const b = 'local  x  =  1\nprint(x) -- 另一条注释\n';
      expect(normalizeScript(a), normalizeScript(b));
    });
  });

  group('splitByUtf8Size', () {
    test('不切断多字节字符', () {
      final chunks = splitByUtf8Size('中文测试内容', 4);
      for (final chunk in chunks) {
        expect(chunk.runes.length * 3, lessThanOrEqualTo(6));
      }
      expect(chunks.join(), '中文测试内容');
    });
  });
}
