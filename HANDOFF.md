# 第一版交接

项目路径为 `/Users/thomastung/GitHub/c3-ble-debugger`。电脑端采用 Python 终端界面，固件为独立 BLE GATT 调试固件。日常使用的 Chromecast 接收器项目位于相邻的 `chromecast-c3-receiver`，vRemoter 位于相邻的 `vRemoter`。

## 已实现

- 电脑端串口选择、hello 身份验证、扫描结果选择、连接、配对、服务枚举、读写、通知订阅及取消订阅。
- 固件单连接 GAP/GATTC 操作，检查实际 GATT 完成状态。
- 按操作标记 JSONL 日志，记录 UTC 时间、主机单调时间和原始通知。
- 错误反馈、命令超时、设备断开和输入大小限制。

## 现有验证

- `python3 -m unittest discover -s tests` 通过，覆盖分帧、响应关联、错误、超时和 PTY 串口整段终端会话。
- UART0 固件构建通过。
- 调试固件 `0002` 在 AlphaPi ESP32-C3 上握手、扫描与状态查询通过，现场扫描返回 118 个设备，状态 dropped=0。
- `0001` 多设备扫描触发 loopTask 栈溢出，`0002` 将大缓冲移入静态区，复验通过。

## 后续会话入口

先读本文件和 README，再检查 Git 状态及 VERSION。不要将板卡当前固件状态当作固定事实，先读串口 hello。测试时退出占用同串口的 vRemoter。日志位于被 Git 忽略的 `logs/`，可能包含附近设备地址，分享时应选择必要内容。

启动电脑端的命令为 `python3 desktop/ble_debugger.py --ports` 与 `python3 desktop/ble_debugger.py --port <PORT>`。用户尚未要求生成安装包。

## 补充实机结果

`0002` 已在目标遥控器验证 secure 连接成功、发现 8 个 GATT 服务、读取 214 字节 HID Report Map、notify 订阅、取消订阅和断开，GATT 状态均成功。第二轮计划录入实际方向键通知时连接超时，尚未收集该工具的实际通知样本。后续优先复验通知数据、长按高频输出、写入远端应答和错误分支。不要把“订阅成功”当成实际通知已验证。

等待连接超时时，固件会拒绝新连接直至控制器报告前一次连接结束。底层 SDK 没有公开的未完成连接取消 API，若之后才连接成功，固件会关闭该过期连接。现场日志在 `logs/connection-wake-0002.jsonl` 及 `logs/notifications*-0002.jsonl`。

首版验收后，现场 AlphaPi 已恢复日常 Chromecast 接收固件 `0006`，vRemoter 已重新启动。下次调试需要重新刷入调试固件。
