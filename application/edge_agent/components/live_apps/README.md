# BLE 固件的天气、AIHOT、股票配套服务

基线为 `c38a2762`（BLE语音、NimBLE和共享HID）。导入用户MVP 0.4.0源码包的
clip_feed、clip_live、clip_live_format和原生clip_ui；保留对应SD卡v4 Lua、manifest、
clip_feeds缓存、pisen-live NVS中的天气地区及股票设置。未导入Classic SPP或手机取景栈。

Wi-Fi由现有wifi_manager唯一管理。仅当新固件app_config尚无SSID时，将已有pisen-live
凭据导入app_config；不会覆盖已有配网或删除原命名空间。数据服务通过状态回调读取连接，
不会重新初始化、停止或反初始化Wi-Fi。地区/自选操作继续使用原服务和原持久化格式。

启动在app_claw前注册外部clip_ui模块，在BLE bridge启动后启动数据线程和按键订阅。
按键按板级标签映射：VOL+/VOL-、MODE、SET；只在clip_ui应用运行时接收，SET经Lua循环
退出及资源清理。保留原首页和卸载机制，将页数容量从16扩至48以容纳已有安装项。

UART `clip_cmd` 提供device_info、app_list/app_get/app_run/app_stop、live_ui_state、
live_action、network_get、feed_get、screen_capture/read/release，用于真实设备读回。
它不提供安装或卸载，也不是旧Classic SPP协议的无线兼容层。旧0.4.0手机端SPP控制、
手机取景及手机补数未因此获得BLE支持；当前三个应用使用设备Wi-Fi和保留缓存。

网络线程以idle优先级运行，避免长TLS计算饿死idle任务触发看门狗；不关闭看门狗。
每批刷新仅复用同一HTTPS origin的已验证连接，跨域即释放；失效复用连接最多重试一次，
重试前清空半包，批次结束释放TLS内存。AIHOT在恢复滚动和抓屏前结算LVGL布局。

宿主回归：`python3 tests/run_host.py`（实际C缓存/解析和HTTP连接代码、ASan/UBSan）。
设备回归：`python3 tests/device_acceptance.py --before BEFORE.json --out OUTPUT --port PORT`，
校验原源码/设置/应用保留、三个运行状态和非空画面，另需独立验证三路实际HTTP刷新及BLE。
目标板验证：导出IDF6.1后在application/edge_agent执行`idf.py build`；烧录前核对实际
分区表，只写入应用分区，保留现有SYSTEM、storage、NVS和SD卡。构建不等同于上板验收。

2026-10-15实机：ble-live-1已仅写应用分区上板，保留原三份v4源码/manifest、四地区和14个自选代码。
三个运行状态/非空抓屏/退出、BLE握手/voice_status/音频特征发现通过；完整录音/ASR未测。
三路首轮HTTP 200：天气131.5秒、股票287.2秒、AIHOT439.2秒（开机单调时间），批量刷新仍慢。
连续监测未见看门狗/崩溃/复位，非长期稳定性验收。产物SHA256
`3b45a99f0b47a97bb92f22776b1ba9efa485a91e71a7995b81a43829aafb9caa`，
证据`/Users/linxiansheng/esp/backclip-backups/ble-live-20261015/rev3/`。
