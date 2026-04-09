# ESP32-S3-BOX-3 豆包 AI 音箱 — 详细设计文档

## 1. 项目概述

### 1.1 项目目标

基于乐鑫 ESP32-S3-BOX-3 开发板，实现一款可与字节跳动"豆包"大模型实时语音对话的 AI 音箱。用户按下按钮后，设备通过 WebSocket 连接豆包服务器，实时采集麦克风音频、Opus 编码上传，同时接收服务器返回的 OGG/Opus 语音流并解码播放。

### 1.2 硬件平台

| 项目 | 规格 |
|------|------|
| SoC | ESP32-S3 双核 Xtensa LX7 @ 240MHz |
| Flash | 16MB QIO |
| PSRAM | 8MB Octal |
| 扬声器编解码器 | ES8311 (I2S, I2C 控制) |
| 麦克风 ADC | ES7210 (2 通道，本项目使用单声道) |
| 显示屏 | 2.4" IPS LCD 320x240 (Phase 2) |
| 按钮 | GPIO0 (CONFIG 按钮, 低电平有效, 内部上拉) |
| 电源 | USB-C 5V |

### 1.3 软件框架

| 组件 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | >= 5.1.0 (开发环境 v5.3.2) | 基础框架 (FreeRTOS, WiFi, TLS, NVS) |
| ESP-BOX-3 BSP | espressif/esp-box-3 | 板载硬件驱动 (I2S, codec, LCD, 按钮) |
| esp_codec_dev | espressif/esp_codec_dev | 音频编解码器抽象层 |
| esp-opus | 78/esp-opus | Opus 编解码库 |
| libogg | georgik/ogg | OGG 容器解封装 |
| esp_websocket_client | espressif/esp_websocket_client | WebSocket 客户端 |
| esp-sr | espressif/esp-sr | 语音识别 (Phase 2: AFE/WakeNet) |
| miniz (ROM) | ESP-IDF 内置 | Gzip 压缩/解压 |
| cJSON | ESP-IDF 内置 | JSON 构建 (手动 snprintf) |

### 1.4 实施阶段

- **Phase 1 (当前)**：核心对话链路 — 按钮触发、WiFi、WebSocket、Opus 音频编解码、实时双向语音
- **Phase 2 (规划)**：ESP-SR AFE (AEC/NS/AGC)、WakeNet 唤醒词、双轨打断、LCD UI、退出意图识别、低功耗模式

---

## 2. 系统架构

### 2.1 总体架构图

```
+---------------------+          WSS (TLS 1.2)          +-------------------+
|   ESP32-S3-BOX-3    | <=============================> |  豆包语音服务器     |
|                     |   openspeech.bytedance.com:443   |                   |
+---------------------+          /api/v3/realtime/       +-------------------+
                                   dialogue

+========================== ESP32-S3 双核 ==========================+
|                                                                    |
|  Core 0 (网络 + 控制)              Core 1 (音频 DSP)               |
|  +------------------+              +---------------------+         |
|  | main_fsm_task    |              | audio_capture_task  |         |
|  | (pri=10, 6KB)    |              | (pri=22, 8KB)       |         |
|  +------------------+              +---------------------+         |
|  +------------------+              +---------------------+         |
|  | ws_tx_task       |              | audio_play_task     |         |
|  | (pri=15, 8KB)    |              | (pri=21, 8KB)       |         |
|  +------------------+              +---------------------+         |
|  +------------------+                                              |
|  | ws_rx (内部)     |              [capture_rb: 32KB PSRAM]        |
|  | esp_ws_client    |              [playback_rb: 64KB PSRAM]       |
|  +------------------+                                              |
+====================================================================+
```

### 2.2 数据流

```
录音链路 (上行):
  ES7210 麦克风
    → I2S RX (16kHz/16bit/mono)
    → esp_codec_dev_read()
    → audio_capture_task
    → capture_rb (32KB PSRAM RingBuffer)
    → ws_tx_task: 累积 960 样本 (60ms)
    → opus_proc_encode() → Opus 帧
    → build_command(CMD_TASK_REQUEST)
    → esp_websocket_client_send_bin()
    → TLS → 豆包服务器

播放链路 (下行):
  豆包服务器
    → TLS → esp_websocket_client 接收
    → ws_event_handler (碎片重组)
    → protocol_parse_response()
    → on_ws_receive() 回调
    → opus_proc_decode_ogg() (OGG 解封装 + Opus 解码)
    → on_decoded_pcm() 回调
    → playback_rb (64KB PSRAM RingBuffer)
    → audio_play_task
    → esp_codec_dev_write()
    → I2S TX (48kHz/16bit/mono)
    → ES8311 扬声器
```

---

## 3. FreeRTOS 任务设计

### 3.1 任务一览

| 任务名 | 函数 | 核心 | 优先级 | 栈大小 | 职责 |
|--------|------|------|--------|--------|------|
| `audio_cap` | `audio_capture_task` | Core 1 | 22 (最高) | 8192B | I2S 麦克风采集 → capture RingBuffer |
| `audio_play` | `audio_play_task` | Core 1 | 21 | 8192B | playback RingBuffer → I2S 扬声器输出 |
| `ws_tx` | `ws_tx_task` | Core 0 | 15 | 8192B | capture RB → Opus 编码 → WebSocket 发送 |
| `main_fsm` | `main_fsm_task` | Core 0 | 10 | 6144B | 状态机调度、按钮检测、会话生命周期 |
| (内部) | esp_websocket_client | Core 0 | - | 8192B | WebSocket 接收、TLS I/O |

**设计原则**：
- Core 1 专用于音频 I/O，保证采集和播放的实时性
- Core 0 处理网络通信和业务逻辑
- 音频采集优先级最高 (22)，防止 I2S FIFO 溢出
- ws_tx 优先级高于 main_fsm，保证音频上传不被状态切换阻塞
