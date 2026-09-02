# ESP32-S3 SparkBot — 豆包实时语音助手

基于 **ESP32-S3** + **ESP-IDF 6.0.1** 的端到端实时语音对话机器人（SparkBot）。

实现效果：麦克风拾音 → 上行给豆包（ByteDance Seeduplex 3.0 全双工语音服务）→ 豆包返回文本 + 合成语音 → 喇叭播放，形成**边说边听、边听边播**的全双工实时对话闭环。同时用 ST7789 屏幕 + LVGL 显示开机画面。

---

## 1. 硬件与引脚

| 器件 | 型号 | 接口 | 说明 |
|------|------|------|------|
| 主控 | ESP32-S3 | — | 双核 Xtensa LX7 |
| 屏幕 | ST7789 (1.3") | SPI2 | 240×240，LVGL 驱动 |
| 音频编解码 | ES8311 | I2S0 + I2C0 | 麦克风 + 喇叭，24 kHz / 16 bit / 单声道 |
| 摄像头 | ATK-OV2640-V1.2 | DVP 8bit + SCCB(I2C0) | RGB565 240×240，模块自带 12 MHz 晶振 |
| 通信 | 板载 WiFi | — | STA 模式，连路由器 |

> ⚠️ **共用 I2C0 总线**：音频 ES8311 和摄像头 OV2640 的 SCCB 并在同一条 I2C0 上
> （SDA=GPIO4，SCL=GPIO5）。`audio.c` 先初始化 I2C0，`camera.c` 复用该总线
> （`pin_sccb_sda/scl = -1`），所以这两个模块的 SDA/SCL 都要接到 GPIO4/5。

### 1.1 屏幕 ST7789（SPI2，来自 `display.c`）

| 信号 | GPIO | 说明 |
|------|------|------|
| SCLK | 21 | SPI 时钟 |
| MOSI (SDA) | 47 | SPI 数据 |
| DC | 38 | 数据/命令选择 |
| CS | 14 | 片选 |
| RST | 2 | 硬件复位 |
| BLK | 46 | 背光（高电平点亮） |
| VCC | 3.3V | 供电 |
| GND | GND | 地 |

### 1.2 音频 ES8311（I2S0 + I2C0，来自 `audio.c`）

| 信号 | GPIO | 方向 |
|------|------|------|
| I2C SDA | 4 | 控制（与摄像头 SCCB 共用） |
| I2C SCL | 5 | 控制（与摄像头 SCCB 共用） |
| I2S MCLK | 45 | 主时钟输出 |
| I2S BCLK | 39 | 位时钟 |
| I2S WS (LRCK) | 41 | 字选 |
| I2S DOUT | 40 | ESP32 → 模块 DIN（播放） |
| I2S DIN | 42 | 模块 DOUT → ESP32（录音） |

### 1.3 摄像头 ATK-OV2640-V1.2（DVP + SCCB，来自 `camera.c`）

| 信号 | GPIO | 说明 |
|------|------|------|
| SCCB SDA | 4 | 与 ES8311 共用 I2C0 |
| SCCB SCL | 5 | 与 ES8311 共用 I2C0 |
| RESET | 15 | 低有效复位 |
| PCLK | 13 | 像素时钟（模块输入给 ESP32） |
| VSYNC | 6 | 帧同步 |
| HREF | 7 | 行参考 |
| D0 | 11 | 数据 bit0 |
| D1 | 9 | 数据 bit1 |
| D2 | 8 | 数据 bit2 |
| D3 | 10 | 数据 bit3 |
| D4 | 12 | 数据 bit4 |
| D5 | 18 | 数据 bit5 |
| D6 | 17 | 数据 bit6 |
| D7 | 16 | 数据 bit7 |
| PWDN | — | 模块外部已拉低（未接 GPIO） |
| XCLK | — | 模块自带 12 MHz 晶振（未接 GPIO） |

### 1.4 GPIO 分配总表（接线速查）

| GPIO | 用途 | 模块 |
|------|------|------|
| 2 | RST | 屏幕 |
| 4 | SDA | 音频 + 摄像头（I2C0 共用） |
| 5 | SCL | 音频 + 摄像头（I2C0 共用） |
| 6 | VSYNC | 摄像头 |
| 7 | HREF | 摄像头 |
| 8 | D2 | 摄像头 |
| 9 | D1 | 摄像头 |
| 10 | D3 | 摄像头 |
| 11 | D0 | 摄像头 |
| 12 | D4 | 摄像头 |
| 13 | PCLK | 摄像头 |
| 14 | CS | 屏幕 |
| 15 | RESET | 摄像头 |
| 16 | D7 | 摄像头 |
| 17 | D6 | 摄像头 |
| 18 | D5 | 摄像头 |
| 21 | SCLK | 屏幕 |
| 38 | DC | 屏幕 |
| 39 | BCLK | 音频 |
| 40 | DOUT | 音频 |
| 41 | WS | 音频 |
| 42 | DIN | 音频 |
| 45 | MCLK | 音频 |
| 46 | BLK | 屏幕 |
| 47 | MOSI | 屏幕 |

> 提示：GPIO6/GPIO8 被摄像头（VSYNC/D2）占用，因此屏幕的 CS/RST 从旧接线迁到了
> GPIO14/GPIO2；同时刻意避开 GPIO43/44（与 USB-UART 芯片电气相连）。

---

## 2. 目录结构

```
main/
├── main.c               # 入口 app_main，按顺序初始化各模块
├── audio.c / .h         # ES8311 + I2S 初始化（esp_codec_dev），读写音频
├── camera.c / .h        # OV2640 摄像头初始化 + 抓帧探测
├── wifi.c / .h          # WiFi STA 连接（阻塞等 IP）
├── doubao_realtime.c/.h # 豆包实时语音客户端（核心，WebSocket 全双工）
├── doubao_secrets.h     # API Key / 音色 / 提示词（git 忽略，需自行创建）
├── doubao_secrets.h.example
├── ui.c / .h            # 开机 UI（黑底 + Logo）
├── bsp_display.c / .h   # esp_lvgl_adapter 兼容层（对应 esp-bsp 接口）
├── display.c / .h       # ST7789 底层 esp_lcd 初始化
└── opus_audio.c / yay_wav.c  # 旧模块，当前未接入 main 流程
```

---

## 3. 依赖组件（idf_component.yml）

- `espressif/esp_lvgl_adapter` — LVGL 显示
- `espressif/esp_codec_dev` — ES8311 编解码器驱动
- `espressif/esp_websocket_client` — WebSocket 客户端
- `espressif/cjson` — JSON 解析/构造
- `espressif/esp_peer` / `78/esp-opus` — 预留（WebRTC / Opus，当前未用）

---

## 4. 启动流程总览（main.c）

`app_main` 按顺序执行，前一步失败会中止后续：

```
app_main()
 ├─ 1. nvs_flash_init()                 # 掉电存储（失败则擦除重试）
 ├─ 2. esp_event_loop_create_default()  # 默认事件循环（WiFi/WebSocket 事件都走这里）
 ├─ 3. app_ui_start()                   # 初始化屏幕 + LVGL，显示开机画面
 ├─ 4. oai_init_audio_capture()         # 初始化 ES8311 + I2S（失败则 return 停止）
 ├─ 5. sparkbot_camera_init()           # 初始化 OV2640 摄像头（复用 I2C0）
 │     └─ sparkbot_camera_capture_probe() # 抓一帧 RGB565 240x240 校验数据
 ├─ 6. oai_wifi()                       # 连 WiFi，阻塞到拿到 IP
 └─ 7. doubao_realtime_start()          # 启动豆包实时语音（建任务 + 连 WebSocket）
```

---

## 5. 各模块详解

### 5.1 UI（ui.c / display.c / bsp_display.c）

- `app_ui_start()`：用 `esp_lvgl_adapter` 初始化 ST7789 + LVGL，打开背光，画黑色全屏背景 + 居中 Logo 图片。
- 注意：LVGL 任务栈被手动加大到 `8KB`（默认 4KB 会在全屏图片渲染时栈溢出复位）。

### 5.2 音频（audio.c）

- `oai_init_audio_capture()`：走 ESP-IDF 6 新驱动（`i2c_master` + `i2s_std`）+ `esp_codec_dev` 框架：
  1. I2C 主总线 → 探测 ES8311（7 位地址 `0x18`/`0x19`）
  2. I2S0 全双工通道（TX 播放 + RX 录音），ESP32 作 I2S 主机
  3. 数据接口 / 控制接口 / ES8311 编解码接口
  4. 以 **24 kHz / 单声道 / 16 bit** 打开
  5. 输出音量 60，麦克风增益 30 dB
- `oai_audio_read()` / `oai_audio_write()`：对 `esp_codec_dev_read/write` 的薄封装，供语音任务调用。

### 5.3 WiFi（wifi.c）

- `oai_wifi()`：STA 模式连 `WIFI_SSID`，注册事件回调；**阻塞** `while(!g_wifi_connected)` 直到拿到 IP。
- 断线时事件回调自动重连（最多 5 次）。

### 5.4 豆包实时语音（doubao_realtime.c）★核心

协议：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`，模型 `1.2.6.1`（Seeduplex 3.0 全双工）。

采样率换算（关键，见 `audio.h`）：

- 编解码器（ES8311）统一跑 **24 kHz**（喇叭回放 24 kHz 原生支持）。
- 上行麦克风：**24 kHz → 降采样到 16 kHz** → Base64 → JSON。

---

## 6. 豆包实时语音的完整数据流

### 6.1 上行（麦克风 → 豆包）

```
麦克风(ES8311) ─24kHz PCM─> oai_audio_read()
    └─> downsample_24k_to_16k()   # 24k → 16k
    └─> mbedtls_base64_encode()
    └─> snprintf() 拼 JSON: {"type":"input_audio_buffer.append","audio":"<base64>"}
    └─> websocket_send_text()     # 每 20ms 一帧
```

### 6.2 下行（豆包 → 喇叭）

```
豆包服务器 ─WebSocket JSON帧─> websocket_event_handler()
    └─> WEBSOCKET_EVENT_DATA → handle_websocket_data()
          └─> 收到完整 JSON → handle_server_message()
                ├─ "response.output_audio.started" → 清空播放队列、重置 PCM 流
                ├─ "response.output_audio.delta"   → enqueue_audio_delta()
                │      ├─ Base64 解码成 24kHz PCM（处理跨帧半个采样点的字节进位）
                │      └─ xQueueSend(s_playback_queue)  # 交给播放任务
                ├─ "response.output_audio.done"    → 收尾
                ├─ "input_audio_transcription.completed" → 打印 user 文本
                ├─ "response.output_text.done"     → 打印 assistant 文本
                └─ "session.created"               → 置 SESSION_READY 位
```

### 6.3 会话建立握手

1. WebSocket 连接成功 → `WEBSOCKET_EVENT_CONNECTED` → 置 `CONNECTED` 位。
2. 上行任务发 `session.create`（带 model、input 16k、output 24k、voice、instructions）。
3. 服务器回 `session.created` → 置 `SESSION_READY` 位 → 上行任务开始持续推音频。

---

## 7. 任务与唤醒模型（重点）

`doubao_realtime_start()` 创建了两个任务，并让 `esp_websocket_client` 内部再建一个任务。三个任务 + 一个事件回调**并发**运行，通过**事件组 bit** 和**队列**互相唤醒，是典型的 FreeRTOS 生产者-消费者模型：

| 任务 / 回调 | 优先级 | 角色 | 初始阻塞点 | 被谁唤醒 |
|------------|--------|------|-----------|---------|
| `doubao_ws`（websocket 内部） | 6 | 连接 + 收发，触发事件回调 | 等网络事件 | 网络 |
| `websocket_event_handler` | — | **生产者**：置位 / 入队 | — | `doubao_ws` 派发事件 |
| `doubao_uplink_task` | 6 | 消费者：采音上行 | 等 `CONNECTED` 位 | 事件回调置 `CONNECTED` 位 |
| `doubao_playback_task` | 6 | 消费者：播放下行 | 等播放队列 | 事件回调 `xQueueSend` |

事件组三个 bit（`doubao_realtime.c`）：

| Bit | 含义 | 由谁置位 |
|-----|------|---------|
| `DOUBAO_CONNECTED_BIT` | WebSocket 已连接 | `WEBSOCKET_EVENT_CONNECTED` |
| `DOUBAO_SESSION_SENT_BIT` | `session.create` 已发 | 上行任务自身 |
| `DOUBAO_SESSION_READY_BIT` | 收到 `session.created` | `handle_server_message` |

### 上行任务 `doubao_uplink_task` 的状态机

```
循环 {
  1. 等 CONNECTED_BIT（唤醒者：连接事件）
  2. 若没发过 session.create → 发送，置 SESSION_SENT_BIT
  3. 等 CONNECTED | SESSION_READY（唤醒者：session.created，超时 10s 重试）
  4. 读麦克风 → 降采样 → base64 → 发 input_audio_buffer.append   # 每 20ms
}
```

### 播放任务 `doubao_playback_task`

```
循环 {
  阻塞收播放队列（唤醒者：音频 delta 入队）
  → 按 20ms 一块 oai_audio_write() 写喇叭
}
```

> **一句话总结唤醒链**：两个任务自己不做轮询，而是靠事件组/队列的阻塞原语挂起；唤醒它们的是 `websocket_event_handler`（由 `esp_websocket_client` 在收到网络事件时回调），而事件的最终源头是**豆包服务器下发的 WebSocket 帧**。

---

## 8. 关于线程创建后"程序跑到哪"

`doubao_realtime_start()` 里 `xTaskCreate` 创建两个任务后，**当前（main）任务不会停**，继续执行：

1. `esp_websocket_client_start()` — 真正发起连接（内部创建 `doubao_ws` 任务）
2. 打印日志、`return ESP_OK` 回到 `app_main`，`app_main` 无后续代码即返回

同时两个新任务被调度器并行拉起并各自阻塞（播放任务等队列、上行任务等事件位）。main 任务优先级是 1，新任务优先级是 6，但因为它们一启动就阻塞，所以不会抢占卡住 main 任务。

---

## 9. 编译与烧录

```bash
# 1. 准备密钥（git 忽略 doubao_secrets.h，需自行创建）
cp main/doubao_secrets.h.example main/doubao_secrets.h
# 编辑 doubao_secrets.h，填入豆包 API Key；同时改 wifi.c 里的 WIFI_SSID/PASSWORD

# 2. 配置目标芯片（默认已设 esp32s3）
idf.py set-target esp32s3

# 3. 编译
idf.py build

# 4. 烧录 + 打开串口日志
idf.py -p <PORT> flash monitor
```

---

## 10. 配置说明

| 文件 | 配置项 | 说明 |
|------|--------|------|
| `main/doubao_secrets.h` | `DOUBAO_API_KEY` | 豆包语音控制台 → API Key 管理 |
| | `DOUBAO_VOICE_ID` | 音色（如 `zh_female_vv_jupiter_bigtts`） |
| | `DOUBAO_SYSTEM_INSTRUCTIONS` | 系统提示词（限制回答口语化、三句话内） |
| `main/wifi.c` | `WIFI_SSID` / `WIFI_PASSWORD` | WiFi 账号密码 |
| `sdkconfig.defaults` | `CONFIG_WS_BUFFER_SIZE=8192` | 豆包 HTTP Upgrade 响应超过默认 1KB，需加大 |

---

## 11. 常见问题排查

- **提示 `API Key not configured`**：`doubao_secrets.h` 未创建或 Key 是占位符。
- **`ES8311 not found`**：检查 I2C 接线（SDA=4, SCL=5）或模块供电。
- **连接超时 / 断连**：确认 WiFi 已连上、API Key 有效、`CONFIG_WS_BUFFER_SIZE` 已加大。
- **播放有杂音/无声**：确认喇叭接线（DOUT=40）与 `output.format` 为 `pcm_s16le` 24kHz 匹配。
