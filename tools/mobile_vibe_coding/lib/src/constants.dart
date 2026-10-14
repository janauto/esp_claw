// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

/// 协议与调参常量。取值与 tools/host_vibe_coding/host_vibe.py 保持一致，
/// 设备端约束见 application/edge_agent/components/ble_host_bridge/src/ble_host_bridge.c。
library;

const String serviceUuid = '7b7d1001-0000-0000-0057-414c43505345';
const String rxUuid = '7b7d1002-0000-0000-0057-414c43505345';
const String txUuid = '7b7d1003-0000-0000-0057-414c43505345';

const String defaultDevicePrefix = 'esp-claw';

/// 请求分片的每片数据字节数。设备端 BLE_HOST_BRIDGE_CHUNK_DATA_MAX 是 96，
/// 但那是响应侧；请求侧的整帧要塞进一次 ATT write，64 是电脑端验证过的值。
const int requestChunkSize = 64;

/// 设备端 preferred MTU。协商更大的值不会有坏处，写入仍按 requestChunkSize 分片。
const int preferredMtu = 247;

/// 设备主动推 job 结果时用的固定请求 id，见 BLE_HOST_BRIDGE_EVENT_REQUEST_ID。
const String deviceEventRequestId = 'job-event';

const Duration defaultTimeout = Duration(seconds: 15);

const int defaultMaxAttempts = 3;

/// 一份完整的 LVGL 游戏脚本 200+ 行，4096 装不下；截断后上传的是半个程序。
const int defaultMaxTokens = 32000;
const int defaultContextWindow = 1000000;

/// 流式请求的"卡住"判定：多久没有收到任何新内容就认为连接断了。
/// 流式下超时语义从"总耗时"变成"静默间隔"，所以可以给得比非流式小得多。
const Duration httpStreamStallTimeout = Duration(seconds: 90);

/// 非流式读超时的下限，以及每 1000 个输出 token 追加的秒数。
const double httpTimeoutMinS = 180.0;
const double httpTimeoutPer1kTokensS = 12.0;
const double httpTimeoutMaxS = 1800.0;

/// 生成期间这么久没有可报的动作就打一行"还活着"，覆盖思维链很长的那段静默。
const Duration streamHeartbeat = Duration(seconds: 15);

/// 这些状态码意味着"服务不认 stream 参数"，退回非流式还有救。
/// 401/403/429 不在其中：鉴权和限流重试一次也一样，白费一个请求。
const Set<int> streamUnsupportedHttpCodes = {400, 404, 422, 501};

/// BLE 轮询间隔优化：从 2.5s 缩短到 1s，提升响应速度。
const Duration jobPollInterval = Duration(milliseconds: 1000);
const Duration jobHeartbeat = Duration(seconds: 15);
/// 默认轮询超时从 90s 缩短到 30s，避免长时间"卡住"。
const Duration jobPollTimeout = Duration(seconds: 30);

const Set<String> terminalJobStatus = {'done', 'failed', 'timeout', 'stopped'};

/// 一次性脚本的执行上限。跑完就该退回主界面，卡住时要有人来收尸。
const int defaultLuaTimeoutMs = 60000;

/// timeout_ms=0 在 cap_lua 异步路径里表示"跑到被取消为止"。游戏/常驻界面必须用
/// 这个值：给任何有限值都等于约好了几秒后把画面掐掉。
const int luaTimeoutUntilCancelled = 0;

/// 界面/游戏脚本至少要跑这么久才算真的把画面撑住了，低于这个值属于"闪一下就退"。
const int longRunMinS = 5;

/// 完整游戏/界面脚本的生成预算下限。只在用户把 maxTokens 调得更低时才起作用。
const int longRunMinTokens = 16000;

/// 规划阶段的 token 上限。给得比生成阶段小，但要留够 thinking 模型的推理余量。
const int plannerMaxTokens = 8000;

const Set<String> longRunTaskKinds = {'game', 'ui', 'animation', 'interactive'};

const List<String> longRunPromptHints = [
  '游戏', '贪吃蛇', '俄罗斯方块', '动画', '界面', '按钮', '菜单',
  '时钟', '表盘', '仪表', '触摸', '点击', '交互', '屏幕上',
];

/// 上下文总字符预算。把 API 文档无节制堆进去会稀释关键约束，反而更容易生成幻觉 API。
const int defaultCharBudget = 48000;

const int maxScriptEcho = 6000;
const int maxLogEcho = 1200;
const int maxRepairRounds = 3;
const int maxPlaybookChars = 9000;

const String flickerDiagnosis =
    '脚本执行后 {runtime}s 就正常退出了，屏幕只闪了一下就回到主界面，设备没有任何日志输出。'
    '这类任务必须常驻运行：脚本末尾要有 `while true do ... lvgl.process_events(16) end` '
    '主循环，或用 lvgl 定时器 + 阻塞等待把脚本撑住。'
    '上一轮只创建了对象/定义了状态表就走到文件末尾，Lua 解释器随即退出，LVGL 对象跟着销毁。'
    '请输出带主循环的完整可运行脚本，不要只给片段。';

/// 判断这次任务是否本该常驻运行。
bool wantsLongRun(String prompt, [String taskKind = '']) {
  if (longRunTaskKinds.contains(taskKind.trim().toLowerCase())) {
    return true;
  }
  return longRunPromptHints.any(prompt.contains);
}

/// 决定这次执行给设备多长的执行窗口。
///
/// [requested] 为 null 表示"按任务类型自动定"，这是默认路径：
/// - 游戏/常驻界面 -> 0（跑到被取消为止）。给有限值会让设备到点把 job 杀掉，
///   表现就是玩着玩着 status=timeout、画面退回主界面。
/// - 一次性脚本 -> 有限上限。这类脚本本来就该执行完退回主界面，
///   给 0 的话写错一个死循环就没人收尸了。
///
/// 显式传值一律照办，包括显式的 0。
int resolveLuaTimeoutMs(int? requested, bool expectsLongRun) {
  if (requested != null) {
    return requested < 0 ? 0 : requested;
  }
  return expectsLongRun ? luaTimeoutUntilCancelled : defaultLuaTimeoutMs;
}

/// 按输出预算放大读超时：token 越多，模型写得越久。
Duration httpTimeoutFor(int maxTokens) {
  final budget = maxTokens < 0 ? 0 : maxTokens;
  final scaled = httpTimeoutMinS + budget / 1000.0 * httpTimeoutPer1kTokensS;
  final seconds = scaled.clamp(httpTimeoutMinS, httpTimeoutMaxS);
  return Duration(milliseconds: (seconds * 1000).round());
}
