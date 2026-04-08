# ESP32-S3-BOX-3 豆包 AI 音箱系统设计文档 (V3.0)

## 1. 系统架构与任务拓扑
系统充分利用 ESP32-S3 的双核特性，将高频 DSP 运算与异步网络通讯（WebSocket）彻底分离，确保语音交互的零卡顿。



### 1.1 任务分工 (FreeRTOS Tasks)
| 任务名称 | 运行核心 | 优先级 | 职责 |
| :--- | :--- | :--- | :--- |
| `Audio_Capture_Task` | **Core 1** | 22 (最高) | I2S 采样、AFE 处理（AEC/NS/AGC）、VAD 检测、唤醒词匹配。 |
| `Audio_Play_Task` | **Core 1** | 21 (高) | 从播放 RingBuffer 取 PCM、I2S 输出、**提供 AEC 参考信号**。 |
| `WS_TX_Task` | **Core 0** | 15 (中) | PCM 封包（8字节 Header + 大端序）、发送 `StartSession` JSON。 |
| `WS_RX_Task` | **Core 0** | 16 (中) | 解析服务器事件（`ASRInfo`, `TTSEnded`）、**Session ID 过滤**。 |
| `Main_FSM_Task` | **Core 0** | 10 (低) | 调度全局状态机、驱动 LCD UI 刷新。 |

---

## 2. 核心交互逻辑：双轨制打断 (Hybrid Barge-in)

为了平衡响应速度与语义准确度，系统设计了两套并行的打断路径。

### 2.1 路径 A：本地强占 (Local Preemption)
* **触发条件**：本地 `esp_sr` (WakeNet) 命中唤醒词 **"Hi Jeson"**。
* **逻辑动作**：
    1. **物理静音**：立即调用 `i2s_zero_dma_buffer`，瞬间切断当前音频输出。
    2. **Session 重置**：`active_session_id++`，并立即发送新的 `StartSession` 指令。
    3. **数据拦截**：`WS_RX_Task` 开始丢弃所有旧 `session_id` 的音频包，防止“尾音”泄露。
* **目的**：消除网络延迟，提供瞬间唤醒的确定性。

### 2.2 路径 B：云端语义打断 (Cloud Feedback)
* **触发条件**：收到服务器推送的 `type: "ASRInfo"` 且包含首字识别。
* **逻辑动作**：
    1. **软打断**：仅清空播放 RingBuffer，不重启 Session，以维持对话上下文。
    2. **状态同步**：将 FSM 切换至 `STATE_LISTENING` 状态。
* **目的**：实现“边说边听”的自然插嘴体验。

---

## 3. 协议封装与会话配置

### 3.1 封包规范 (Binary Protocol)
* **Header (8 Bytes)**:
    * `Byte 0`: `(version << 4) | header_size` $\rightarrow$ 固定值 `0x18`。
    * `Byte 4-7`: `payload_len` $\rightarrow$ 使用 `htonl()` 转换为大端序。
* **Payload**: 16bit / 16kHz / 单声道 PCM，每包长度建议 20ms - 40ms。

### 3.2 `StartSession` 关键参数配置
```json
{
    "asr": {
        "extra": {
            "end_smooth_window_ms": 800,
            "enable_custom_vad": true
        }
    },
    "dialog": {
        "bot_name": "Jeson",
        "extra": {
            "model": "1.2.1.1",
            "enable_user_query_exit": true,
            "input_mod": "microphone"
        }
    }
}
```

---

## 4. 特殊事件处理：退出意图识别
系统通过监控 `TTSEnded` 事件中的 `status_code` 实现自动休眠。

* **触发码**：`"20000002"`（用户表达了“再见”、“不说了”等退出意图）。
* **系统响应**：
    1. 播报完最后的结束语。
    2. 状态机强制跳转至 `STATE_IDLE`。
    3. **停止录音上传**，进入低功耗待命模式，直至下一次本地唤醒。



---

## 5. 内存与性能优化策略
* **内存分配**：所有音频 RingBuffer 及 WebSocket 发送/接收缓冲区必须申请在 **PSRAM** (`MALLOC_CAP_SPIRAM`)，防止堆内存溢出。
* **指令加速**：必须在 `sdkconfig` 中开启 S3 的 **AI 指令集优化**，确保 Core 1 处理 AFE 算法时的 CPU 占用率低于 50%。
* **数据对齐**：确保 I2S 录音与播放的采样率严格匹配 (16kHz)，否则 AEC (回声消除) 效果会大幅下降。

---

## 6. 分区表定义 (`partitions.csv`)
```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x4000,
otadata,  data, ota,     ,        0x2000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        4M,
model,    data, spiffs,  ,        6M,     # 存放 WakeNet/MultiNet 算法模型
storage,  data, spiffs,  ,        1M,
```


