# C3 BLE Debugger

浏览器 Web 调试台与 ESP32-C3 BLE 调试固件。浏览器通过 Web Serial 直接连接 C3，用于配对设备、逐项采样按键/开关/滚轮、查看去重键值并导出 CSV。

## 目录

- `firmware/` — ESP32-C3 BLE 调试固件，独立于日常遥控器接收固件。
- `desktop/web/` — 浏览器直连串口的 Web 调试台，无需后台服务。
- `tests/` — Web 串口协议、采样和界面状态回归测试。
- `VERSION` — 四位编译号。

## Web 按键采样台

使用桌面 Chrome 直接打开 `desktop/web/index.html`。无需安装运行环境或启动本地服务，页面使用 Web Serial 直接访问 USB 串口。所有资源随仓库提供，不依赖 CDN。必须在具备 Web Serial 的安全上下文中打开；已在 Chrome 的本地 `file://` 页面验证能力可用。浏览器不支持时页面会提示。

页面分为「01 连接设备」「02 测试」「03 结果导出」三个 Tab，按键数量与逐键采样同处测试页。切换只改变可见面板，不中断连接和正在进行的采样，结果与清单保留。

1. 关闭 vRemoter 或其他串口工具，点击「选择 C3 串口」，由浏览器授权 AlphaPi 对应的串口。
2. 已配对设备在连接串口后自动显示，可重命名；备注仅保存在当前浏览器，新建测试不会清除。选择已配对设备并连接，或让新设备进入配对模式后扫描、连接并配对。已有绑定未保存地址类型时，可在列表选择 public / random。
3. 连接后自动枚举服务并订阅通知；开始采样前也会补齐缺失的通道准备，进度和失败原因显示在设备列表下方；如果串口接管的是旧蓝牙连接，会自动重连并准备通道；可展开通道列表查看失败原因、切换通道。默认优先前四个 HID 通知通道，不能代表覆盖所有按键；未收到报文时需检查通道选择。
4. 输入按键数量（1–100），生成清单；逐个命名、开始采样、操作实体按键、停止并保存。自动记录「收到报文 / 未收到报文」，切换到下一键，不需要确认通过或跳过；支持重新采样，保留各次记录。停止后可清空本键全部采样（保留名称），或新建测试清空整轮记录。
5. 导出 CSV，页面结果表和 CSV 仅「按键、键值」两列。开关、滚轮各算一项，同次采样可有多个键值；按 HEX 去重、保持首次出现顺序，使用最近一次采样，不混入旧的尝试。原始报文不去重。

页面移除了手动刷新状态与读取已配对设备按钮；「重置串口」会关闭并重新打开已选串口、验证固件并更新设备列表，无需重新弹出串口选择器，不重启 C3 固件。

只记录点击开始之后的通知。断线或固件丢包事件会中止当前采样并标记异常。刷新会关闭串口并将未结束的采样标为异常；结果保存在当前浏览器存储，不保证跨浏览器或移动文件路径后共享，请及时导出。本地存储满时会提示。单次采样上限 10000 条，串口追踪保留最近 20000 条，追踪截断状态记录在浏览器本地；按键样本另行保留。

Web 版本为 `0010`，板上调试固件仍为 `0002`，页面分别显示。Web 已实测串口握手、已有绑定加密重连、服务枚举和 4 路通知订阅；新设备配对及不同开关、滚轮的完整采样仍需对应设备验收。

```sh
node --test tests/test_web*.js
```

## 范围

通过 Web 界面操作。支持 BLE GATT 单设备连接，经典蓝牙和厂家专有 2.4 GHz 协议需要对应硬件和协议适配。该工具输出原始数据，按键含义需要结合描述符和实测确定。

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

当前首版编译号为 `0002`。UART0 已实测握手、扫描、加密连接、服务枚举、HID 描述符读取及订阅配置。实际按键通知采样已验证，任意特征写入仍需后续硬件验收，详见 `HANDOFF.md`。
