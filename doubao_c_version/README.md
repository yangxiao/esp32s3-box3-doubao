# Doubao Realtime Dialog - C Version

豆包实时对话 API 的 C 语言实现，功能对标 `doubao_py_demo` Python 版本。

## 音频格式

- **输入（麦克风/文本模式）**：麦克风采集 16kHz PCM，本地 Opus 编码后上传（`speech_opus`），大幅降低带宽
- **输入（音频文件模式）**：直接发送 PCM（16kHz, 16bit, 单声道, 小端序）
- **输出（TTS）**：服务端返回 OGG/Opus 音频，本地解封装 + Opus 解码为 48kHz PCM 播放

## 依赖

- **libwebsockets** - WebSocket 客户端
- **portaudio** - 跨平台音频 I/O（麦克风 + 扬声器）
- **opus** - Opus 音频编解码
- **libogg** - OGG 容器解封装
- **zlib** - gzip 压缩/解压
- **OpenSSL** - SSL/TLS（libwebsockets 依赖）
- **CMake** >= 3.10

### macOS 安装依赖

```bash
brew install cmake libwebsockets portaudio opus libogg
```

### Ubuntu/Debian 安装依赖

```bash
sudo apt install cmake libwebsockets-dev portaudio19-dev libopus-dev libogg-dev zlib1g-dev libssl-dev
```

## 编译

```bash
cd doubao_c_version
mkdir build && cd build
cmake .. && make
```

## 运行

需要设置环境变量 `DOUBAO_APP_ID` 和 `DOUBAO_ACCESS_KEY`：

```bash
export DOUBAO_APP_ID="your_app_id"
export DOUBAO_ACCESS_KEY="your_access_key"
```

### 麦克风模式（默认）

```bash
./doubao_client
```

### 文本模式

```bash
./doubao_client --mod text
```

### 音频文件模式

```bash
./doubao_client --audio /path/to/input.wav
```

### 参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--audio` | 无 | 输入音频文件路径（WAV 格式，PCM 直传） |
| `--mod` | `audio` | 输入模式：`audio`（麦克风）或 `text`（文本） |
| `--recv_timeout` | `10` | 接收超时（秒），范围 10-120 |

## 文件结构

| 文件 | 说明 |
|------|------|
| `config.h` | 配置定义、UUID 生成、会话 JSON 构建 |
| `protocol.h/c` | 二进制协议头生成与响应解析、gzip 压缩解压 |
| `client.h/c` | libwebsockets WebSocket 客户端 |
| `audio.h/c` | PortAudio 音频 I/O、Opus 编解码、OGG 解封装、线程安全播放队列 |
| `main.c` | 入口、参数解析、会话管理、事件处理 |
| `CMakeLists.txt` | CMake 构建配置 |

## 协议流程

```
1. WebSocket 连接（携带自定义 Headers）
2. StartConnection (cmd=1)
3. StartSession (cmd=100) + 配置 JSON（ASR: speech_opus, TTS: ogg_opus）
4. SayHello (cmd=300) → 接收 OGG/Opus TTS 欢迎语 → 解码播放
5. 麦克风 PCM → Opus 编码 → 发送 (cmd=200) / 文本查询 (cmd=501)
6. 接收 OGG/Opus 音频回复 → 解码播放 + 事件处理
7. FinishSession (cmd=102) → FinishConnection (cmd=2)
```
