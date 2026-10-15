# clip_ui

移植自用户MVP 0.4.0源码，用于480×800天气、AIHOT和股票v4应用。
先`local ui=require("clip_ui")`，再`ui.open(title, color)`；循环中调用
`ui.running()`、`ui.key()`和`ui.feed_dashboard(kind, json, status, key)`，
kind为weather、aihot或stocks。`ui.feed(kind)`返回缓存JSON和age_ms，失败返回nil/error。
`ui.feed_refresh(kind)`请求有频率限制的刷新；`ui.feed_refresh_requested()`读取按钮请求。
`ui.millis()`返回单调毫秒。旧card/stock_dashboard/stock_selection等API保留。

通过display_service独占LVGL屏幕；不可与另一LVGL/display应用同时持有显示资源。
应用必须在循环结束后`ui.close()`，退出清理回调也会释放画面、字库和display session。
SET会请求退出，下一次running返回false。原生注册接口由live_apps服务连接实际缓存、
联网和按键；不模拟Wi-Fi或手机通信，不要求更改原v4 Lua源码。
