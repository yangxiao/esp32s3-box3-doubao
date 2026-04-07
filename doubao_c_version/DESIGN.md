# 豆包实时对话客户端 - 设计文档

## 1. 项目概述

豆包实时对话客户端是一个用C语言实现的实时语音对话系统，支持与字节跳动豆包API进行音频和文本交互。

### 主要功能
- 实时语音对话（麦克风输入 + TTS输出）
- 文本输入对话
- 音频文件输入处理
- Opus编解码支持
- WebSocket安全连接

---

## 2. 系统架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                         main.c                                │
│                    会话管理层 (session_t)                     │
├─────────────────────────────────────────────────────────────┤
│  ┌────────────────┐    ┌──────────────────┐                 │
│  │   client.c     │    │     audio.c      │                 │
│  │  WebSocket层   │    │   音频管理模块    │                 │
│  └────────────────┘    └──────────────────┘                 │
├─────────────────────────────────────────────────────────────┤
│                      protocol.c                               │
│                   协议编解码层                                 │
├─────────────────────────────────────────────────────────────┤
│                       config.h                                 │
│                    配置与常量定义                              │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 模块划分

| 模块 | 文件名 | 职责 |
|------|--------|------|
| 会话管理 | main.c | 应用入口，线程协调，会话状态管理 |
| WebSocket客户端 | client.h/client.c | WebSocket连接管理，消息队列 |
| 音频管理 | audio.h/audio.c | 麦克风输入、扬声器输出、Opus编解码 |
| 协议处理 | protocol.h/protocol.c | 二进制协议编解码，gzip压缩 |
| 配置 | config.h | 服务器配置、音频参数、会话配置 |

---

## 3. 核心数据结构

### 3.1 会话状态 `session_t` (main.c:15-33)

```c
typedef struct {
    doubao_client_t client;        // WebSocket客户端
    audio_manager_t audio;          // 音频管理器
    char output_format[32];         // 输出音频格式
    char input_mod[32];             // 输入模式 (audio/text/audio_file)
    char audio_file[512];           // 音频文件路径
    int recv_timeout;               // 接收超时

    // 状态标志
    volatile bool running;
    volatile bool session_finished;
    volatile bool user_querying;
    volatile bool say_hello_done;
    volatile bool has_audio;

    // 线程同步
    pthread_t service_thread;       // WebSocket服务线程
    pthread_t input_thread;         // 输入线程
    pthread_mutex_t state_mutex;
    pthread_cond_t hello_cond;
} session_t;
```

### 3.2 WebSocket客户端 `doubao_client_t` (client.h:24-67)

```c
typedef struct {
    struct lws_context *context;    // libwebsockets上下文
    struct lws *wsi;                // WebSocket连接句柄
    char host[256];
    int port;
    char path[512];

    // 认证头
    char app_id[128];
    char access_key[128];
    char resource_id[128];
    char app_key[64];
    char connect_id[64];

    // 会话配置
    char session_id[128];
    char output_format[32];
    char input_mod[32];
    char asr_audio_format[32];
    bool use_opus_input;
    int recv_timeout;

    // 发送队列（环形缓冲区）
    ws_message_t send_queue[CLIENT_SEND_QUEUE_SIZE];
    int send_head;
    int send_tail;
    pthread_mutex_t send_mutex;

    // 接收缓冲区
    uint8_t *recv_buf;
    size_t recv_buf_len;
    size_t recv_buf_cap;

    // 回调
    recv_callback_t on_recv;
    void *recv_userdata;

    // 状态
    volatile bool connected;
    volatile bool running;
    char logid[256];
} doubao_client_t;
```

### 3.3 音频管理器 `audio_manager_t` (audio.h:29-68)

```c
typedef struct {
    // PortAudio流
    PaStream *input_stream;
    PaStream *output_stream;

    // 输入配置
    int input_sample_rate;      // 16000 Hz
    int input_channels;         // 1
    int input_chunk;            // Opus帧大小 (960帧 = 60ms)
    PaSampleFormat input_format; // paInt16

    // 输出配置
    int output_sample_rate;     // 48000 Hz
    int output_channels;        // 1
    int output_chunk;           // 2880帧 = 60ms
    PaSampleFormat output_format; // paInt16

    // Opus编解码器
    OpusEncoder *opus_encoder;
    OpusDecoder *opus_decoder;
    bool opus_decoder_inited;

    // OGG解复用状态
    ogg_sync_state ogg_sync;
    ogg_stream_state ogg_stream;
    bool ogg_stream_inited;
    bool ogg_headers_parsed;
    int ogg_header_count;

    // 播放队列和线程
    audio_queue_t play_queue;
    pthread_t player_thread;
    volatile bool playing;

    // 原始OGG缓冲区（用于保存文件）
    uint8_t *ogg_buffer;
    size_t ogg_buffer_len;
    size_t ogg_buffer_cap;
} audio_manager_t;
```

### 3.4 协议解析结果 `parsed_response_t` (protocol.h:56-68)

```c
typedef struct {
    int message_type;           // 消息类型
    int event;                  // 事件代码，-1表示无
    int has_event;
    uint32_t seq;
    int has_seq;
    char session_id[128];
    uint32_t payload_size;
    uint8_t *payload_data;      // 解压后的数据（调用者需释放）
    size_t payload_data_len;
    int is_binary;              // 1=原始音频，0=JSON文本
    uint32_t error_code;
} parsed_response_t;
```

---

## 4. 协议格式

### 4.1 协议头格式 (4字节)

```
字节0: [版本(4位) | 头大小(4位)]
字节1: [消息类型(4位) | 标志(4位)]
字节2: [序列化方式(4位) | 压缩方式(4位)]
字节3: 保留
```

### 4.2 消息类型 (protocol.h:11-16)

| 值 | 定义 | 说明 |
|----|------|------|
| 0x01 | MSG_CLIENT_FULL_REQUEST | 客户端完整请求 |
| 0x02 | MSG_CLIENT_AUDIO_ONLY | 客户端纯音频数据 |
| 0x09 | MSG_SERVER_FULL_RESPONSE | 服务器完整响应 |
| 0x0B | MSG_SERVER_ACK | 服务器确认（音频数据） |
| 0x0F | MSG_SERVER_ERROR | 服务器错误 |

### 4.3 命令ID (protocol.h:36-45)

| 值 | 定义 | 说明 |
|----|------|------|
| 1 | CMD_START_CONNECTION | 开始连接 |
| 2 | CMD_FINISH_CONNECTION | 结束连接 |
| 100 | CMD_START_SESSION | 开始会话 |
| 102 | CMD_FINISH_SESSION | 结束会话 |
| 200 | CMD_TASK_REQUEST | 任务请求（音频数据） |
| 300 | CMD_SAY_HELLO | 问候语 |
| 500 | CMD_CHAT_TTS_TEXT | 聊天TTS文本 |
| 501 | CMD_CHAT_TEXT_QUERY | 聊天文本查询 |

### 4.4 事件代码 (protocol.h:47-53)

| 值 | 定义 | 说明 |
|----|------|------|
| 350 | EVENT_TTS_TEXT_CLEAR | TTS文本清除 |
| 359 | EVENT_TTS_ENDED | TTS结束 |
| 450 | EVENT_CLEAR_CACHE | 清除缓存 |
| 459 | EVENT_USER_QUERY_END | 用户查询结束 |
| 152, 153 | EVENT_SESSION_FINISH_1/2 | 会话结束 |

---

## 5. 线程模型

### 5.1 线程架构

```
主线程 (main)
  ├─ service_thread ──┐
  │                   ├─ WebSocket事件循环 (lws_service)
  │                   └─ 消息发送/接收处理
  │
  └─ input_thread ─────┬─ (audio mode) 麦克风读取 + Opus编码
                       └─ (text mode)  文本输入读取
                      
  (独立) player_thread ── 播放队列消费 + PortAudio输出
```

### 5.2 线程同步机制

| 机制 | 用途 | 位置 |
|------|------|------|
| `pthread_mutex_t send_mutex` | 保护发送队列 | client.c |
| `pthread_mutex_t state_mutex` | 保护会话状态 | main.c |
| `pthread_cond_t hello_cond` | 等待say_hello完成 | main.c |
| `pthread_mutex_t play_queue.mutex` | 保护播放队列 | audio.c |
| `pthread_cond_t play_queue.cond` | 播放队列等待 | audio.c |

---

## 6. 会话流程

### 6.1 正常会话时序

```
Client                              Server
  |                                   |
  |--- StartConnection -------------->|
  |                                   |
  |<-- ACK (with logid) -------------|
  |                                   |
  |--- StartSession ----------------->|
  |    (ASR/TTS配置)                 |
  |                                   |
  |--- SayHello --------------------->|
  |                                   |
  |<-- TTS音频流 (MSG_SERVER_ACK) ---|
  |<-- EVENT_TTS_ENDED (359) --------|
  |                                   |
  |--- 音频数据 --------------------->|
  |   (MSG_CLIENT_AUDIO_ONLY)        |
  |                                   |
  |<-- 文本响应 ---------------------|
  |<-- TTS音频流 --------------------|
  |<-- EVENT_USER_QUERY_END (459) --|
  |<-- EVENT_TTS_ENDED (359) --------|
  |                                   |
  |--- FinishSession ---------------->|
  |<-- EVENT_SESSION_FINISH ---------|
  |                                   |
  |--- FinishConnection ------------->|
```

### 6.2 事件处理流程 (main.c:46-110)

```
on_ws_receive()
  |
  +-- protocol_parse_response()
  |
  +-- 消息类型分支
      |
      +-- MSG_SERVER_ACK (二进制)
      |   └─ audio_decode_ogg_opus() → 播放队列
      |
      +-- MSG_SERVER_FULL_RESPONSE
      |   ├─ 打印文本响应
      |   └─ 事件处理
      |       ├─ EVENT_CLEAR_CACHE (450): 清空音频队列
      |       ├─ EVENT_USER_QUERY_END (459): 标记查询结束
      |       ├─ EVENT_TTS_ENDED (359): 标记hello完成
      |       └─ EVENT_SESSION_FINISH: 结束会话
      |
      └-- MSG_SERVER_ERROR
          └─ 打印错误并退出
```

---

## 7. 音频处理流程

### 7.1 音频参数配置 (config.h:9-19)

| 参数 | 值 | 说明 |
|------|-----|------|
| INPUT_SAMPLE_RATE | 16000 Hz | 麦克风采样率 |
| INPUT_CHANNELS | 1 | 单声道 |
| OPUS_FRAME_SIZE | 960帧 | 60ms @ 16kHz |
| OUTPUT_SAMPLE_RATE | 48000 Hz | 扬声器采样率 |
| OUTPUT_CHANNELS | 1 | 单声道 |
| Opus编码比特率 | 32000 bps | VOIP模式 |

### 7.2 输入音频流 (麦克风 → 服务器)

```
PortAudio (16kHz, int16, mono)
         ↓
   PCM帧 (960 samples)
         ↓
  Opus编码 (32kbps)
         ↓
  client_task_request()
         ↓
  WebSocket发送
   (MSG_CLIENT_AUDIO_ONLY, 无压缩)
```

### 7.3 输出音频流 (服务器 → 扬声器)

```
  WebSocket接收
  (MSG_SERVER_ACK, OGG/Opus)
         ↓
  ogg_buffer_append() [保存原始数据]
         ↓
  ogg_sync_pageout() → ogg_stream_packetout()
         ↓
  跳过前2个包头 (OpusHead + OpusTags)
         ↓
  Opus解码 (48kHz, int16, mono)
         ↓
  queue_push() → 播放队列
         ↓
  player_thread 消费
         ↓
  PortAudio输出
```

---

## 8. 编译与依赖

### 8.1 依赖库 (CMakeLists.txt:6-25)

| 库 | 用途 | 版本要求 |
|----|------|---------|
| libwebsockets | WebSocket通信 | >= 4.0 |
| portaudio-2.0 | 音频I/O | >= 19 |
| opus | 音频编解码 | >= 1.3 |
| ogg | OGG容器解复用 | >= 1.3 |
| OpenSSL | TLS加密 | 任意 |
| zlib | gzip压缩 | 任意 |
| pthread | 线程支持 | 系统自带 |

### 8.2 编译命令

```bash
mkdir build && cd build
cmake ..
make
```

---

## 9. 配置与运行

### 9.1 环境变量

| 变量 | 必须 | 说明 |
|------|------|------|
| DOUBAO_APP_ID | 是 | 应用ID |
| DOUBAO_ACCESS_KEY | 是 | 访问密钥 |

### 9.2 命令行参数 (main.c:206-214)

| 参数 | 说明 | 默认值 |
|------|------|--------|
| --format <fmt> | 输出音频格式 | pcm |
| --audio <file> | 音频文件输入 | - |
| --mod <mode> | 输入模式 (audio/text) | audio |
| --recv_timeout <N> | 接收超时 (10-120秒) | 10 |
| --help | 显示帮助 | - |

### 9.3 会话配置 (config.h:50-97)

```c
// TTS配置
"tts": {
  "speaker": "zh_female_vv_jupiter_bigtts",
  "audio_config": {
    "channel": 1,
    "format": "ogg_opus",
    "sample_rate": 24000
  }
}

// 对话配置
"dialog": {
  "bot_name": "豆包",
  "system_role": "你使用活泼灵动的女声...",
  "speaking_style": "你的说话风格简洁明了...",
  "location": {"city": "北京"},
  "extra": {
    "strict_audit": false,
    "recv_timeout": 10,
    "input_mod": "audio"
  }
}
```

---

## 10. 关键设计决策

### 10.1 Opus vs PCM

- **输入**: 麦克风使用Opus编码（32kbps），大幅减少带宽
- **输出**: 服务器返回OGG/Opus，高质量低延迟
- **音频文件**: 直接发送PCM（兼容性考虑）

### 10.2 环形发送队列 (client.h:47-49)

- 固定大小64条消息
- 无锁读取，有锁写入
- 避免动态内存碎片

### 10.3 独立播放线程 (audio.c:95-109)

- 从队列消费PCM数据
- 阻塞式等待，无数据时休眠
- 使用PortAudio同步写入

### 10.4 libwebsockets回调模型

- 所有WebSocket操作在lws_service循环中执行
- 发送消息通过环形队列异步排队
- 接收消息在回调中累积，完整后处理

---

## 11. 文件清单

| 文件 | 行数 | 说明 |
|------|------|------|
| main.c | 381 | 主程序，会话管理 |
| client.c | 456 | WebSocket客户端实现 |
| audio.c | 399 | 音频管理实现 |
| protocol.c | 211 | 协议编解码实现 |
| client.h | 103 | 客户端接口定义 |
| audio.h | 103 | 音频管理接口定义 |
| protocol.h | 100 | 协议定义 |
| config.h | 100 | 配置定义 |
| CMakeLists.txt | 65 | 构建配置 |

**总计**: 约2318行代码
