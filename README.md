# C3 BLE Debugger

ESP32-C3 BLE 调试固件与电脑端终端工具。用于扫描设备、检查 GATT 服务、读取报告描述符、订阅原始通知，并按测试动作记录报文。支持 macOS、Windows 和 Linux 的 Python 终端环境。

## 目录

- `firmware/` — ESP32-C3 BLE 调试固件，独立于日常遥控器接收固件。
- `desktop/ble_debugger.py` — 电脑端终端程序。
- `tests/` — 串口分帧和命令响应回归测试。
- `VERSION` — 四位编译号。

## 电脑端

需要 Python 3.9 或更高版本。

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r desktop/requirements.txt
.venv/bin/python desktop/ble_debugger.py --ports
.venv/bin/python desktop/ble_debugger.py --port /dev/cu.usbmodemEXAMPLE
```

Windows 使用 `.venv\Scripts\python.exe`，端口例如 `COM3`。

启动时通过 `hello` 确认固件身份。选错端口或运行日常接收器固件时会提示超时。请先退出占用同一串口的 vRemoter 或串口监视器。

## 适配流程

```text
scan 5
connect 1 secure
discover
read 0x0042
subscribe 0x002b
mark up
mark down
unsubscribe 0x002b
disconnect
quit
```

`connect 1` 使用最近一次扫描结果中的第一个设备及其地址类型。也可以直接指定 `connect AA:BB:CC:DD:EE:FF random secure`。`secure` 表示连接后协商加密与绑定，未添加时仅建立 GATT 连接，可随后用 `pair` 请求配对。目标设备需要进入可发现或配对状态。

`discover` 后按实际发现的服务和特征选择句柄。上面的 `0x0042` 和 `0x002b` 仅为操作示例，其他设备的句柄可能完全不同。标准 BLE HID Report Map 特征的 UUID 为 `2A4B`，可读取其对应特征句柄，用于理解报告格式。

用 `mark 按键名称` 给随后记录加标签，再按硬件按键。程序持续显示原始通知，包括十六进制载荷和句柄，便于对比按下、松开、长按和组合键。

```text
write 0x0012 0100
write 0x0012 0100 no-response
subscribe 0x0030 indicate
bonds
forget AA:BB:CC:DD:EE:FF
status
help
```

写命令应根据目标设备协议选择特征、载荷和写入方式。`no-response` 只能确认本地栈接受请求，实际行为需观察设备响应。超时后先查 `status`，避免自动重发写入。空回车不会重复上一条命令。

## 记录与验证

每次会话自动新建 `logs/capture-*.jsonl`，包含 UTC 时间、主机单调时间、当前标签、命令响应及异步通知。可用 `--log 新文件路径` 指定位置，已存在的文件不会被覆盖。日志可能包含设备地址和操作数据，请自行选择需要分享的记录。

```sh
python3 -m unittest discover -s tests
python3 -m py_compile desktop/ble_debugger.py
```

## 范围

首版提供终端交互。支持 BLE GATT 单设备连接，经典蓝牙和厂家专有 2.4 GHz 协议需要对应硬件和协议适配。该工具输出原始数据，按键含义需要结合描述符和实测确定。

## 调试固件编译与刷写

使用 Arduino CLI 和 Arduino-ESP32 `2.0.14`。固件通过 ESP-IDF 原生 GAP/GATTC API 获取实际 GATT 状态，JSON 由 SDK 内置 cJSON 编解码。

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=default,FlashMode=dio,FlashFreq=40,FlashSize=4M,PartitionScheme=huge_app --output-dir build/uart firmware/C3BLEDebugger
arduino-cli upload --fqbn esp32:esp32:esp32c3:CDCOnBoot=default,FlashMode=dio,FlashFreq=40,FlashSize=4M,PartitionScheme=huge_app --port "<PORT>" --input-dir build/uart firmware/C3BLEDebugger
```

板卡原生 USB 连接时，把 `CDCOnBoot=default` 改为 `CDCOnBoot=cdc`。首版原生 USB 的实机兼容性需要在对应板卡验收。

## 串口协议

请求和响应各占一行 JSON，默认 `115200`。请求带整数 `id` 和 `cmd`，响应复用 `id`，以 `ok` 表示成功与否。异步通知使用 `event` 字段，不会消费命令响应。

```json
{"id":1,"cmd":"hello"}
{"id":2,"cmd":"scan","seconds":5}
{"id":3,"cmd":"connect","address":"aa:bb:cc:dd:ee:ff","address_type":"public","secure":true}
{"id":4,"cmd":"discover"}
{"id":5,"cmd":"subscribe","handle":43,"mode":"notify"}
```

GATT 操作成功的 `data.status` 为 `0`。有响应写入、读操作及 CCCD 配置等待远端 GATT 状态，失败时 `ok=false`。无响应写入的 `confirmed=false`。`subscribe` 必须先在当前连接执行 `discover`，以便校验属性和 CCCD。

首版扫描最多保留 128 个结果，GATT 枚举最多 256 个条目。写入最大 128 字节，并受当前 ATT MTU 限制。通知队列有界，溢出会产生 `dropped` 事件，记录该情况后应缩小订阅范围或降低事件速率。首版支持无输入输出的 Just Works 配对，需要输入 PIN 或数字确认的设备会明确报错。

当前首版编译号为 `0002`。UART0 已实测握手、扫描、加密连接、服务枚举、HID 描述符读取及订阅配置。实际通知采样和任意特征写入仍需后续硬件验收，详见 `HANDOFF.md`。
