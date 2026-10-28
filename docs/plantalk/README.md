# 植语 PlantTalk — 技术整合文档

> 基于 xiaozhi-esp32 v1.9.4 二次开发，面向 ESP32-S3 粤嵌（GEC）开发板的 AI 语音交互智能绿植养护系统。
> 本文档整合所有自研模块的设计说明、协议细节、线程安全规范与踩坑记录，便于后续维护与面试复习。

---

## 1. 项目概述

| 维度 | 说明 |
|---|---|
| 硬件 | ESP32-S3 + GC0308 摄像头 + 电容土壤湿度（预留） + BH1750 光照（预留） + DHT11（虚拟） |
| 框架 | xiaozhi-esp32 v1.9.4-gec 开源固件（MIT 协议） |
| 驱动 | ESP-IDF v5.5.3（**5.4 以上版本必选，v5.1.x 会编译失败**） |
| UI | LVGL v9（深绿主题，2×2 卡片看板） |
| 通信 | WebSocket（AI 对话）+ TCP 巴法云（远程控制） |
| 协议 | MCP（Model Context Protocol）设备侧工具链 |
| 构建工具 | CMake + Ninja + xtensa-esp-elf-gcc v14.2.0 |
| 板卡 | 粤嵌 GEC-ESP32S3-Board（PCA9557 I²C 扩展 GC0308 电源） |
| 状态指示 | GPIO10 = 浇水/补光状态 LED |

---

## 2. 系统架构

```
┌────────────────────────────────────────────────────────────────────────┐
│                        「植语」设备侧（ESP32-S3）                       │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│   ┌──────────────┐    ┌──────────────────┐    ┌─────────────────────┐  │
│   │ AI 聊天界面  │◄──►│   Application    │◄──►│  植物数据看板       │  │
│   │  (LVGL v9)   │    │    主循环 + Tick  │    │  (LVGL v9 2×2 卡片) │  │
│   └──────┬───────┘    └──────┬───────────┘    └──────────┬──────────┘  │
│          │                   │                           │             │
│          │        ┌──────────┼──────────┐                │             │
│          │        │          │          │                │             │
│          ▼        ▼          ▼          ▼                ▼             │
│   ┌──────────┐ ┌──────┐ ┌─────────┐ ┌───────────┐  ┌───────────┐     │
│   │ MCP 工具 │ │ Plant│ │ Bemfa   │ │  Dht11    │  │ led_ctrl  │     │
│   │  注册表  │ │Monitor│ │ Cloud   │ │  Sensor   │  │  GPIO10   │     │
│   │(7 个绿植)│ │自动养护│ │TCP长连接 │ │虚拟温湿度 │  │ 状态指示  │     │
│   └─────┬────┘ └──┬───┘ └────┬────┘ └────┬──────┘  └───────────┘     │
│         │         │          │          │                              │
│         └─────────┼──────────┼──────────┘                              │
│                   │          │                                         │
└───────────────────┼──────────┼────────────────────────────────────────┘
                    │          │
           WebSocket         TCP :8344
           （AI 对话）        （巴法云远程）
                    │          │
                    ▼          ▼
          ┌──────────┐  ┌──────────┐
          │ 小智云端  │  │ bemfa.com│
          │ (MCP 服务)│  │  TCP 创客 │
          └──────────┘  └──────────┘
```

### 三种动作触发源（统一记录到养护日志）

| 触发源 | 示例 | 标签 |
|---|---|---|
| AI 语音对话 | "小智，帮我浇个水" | `"开始浇水"` / `"浇水完成"` |
| 自动养护闭环 | 土壤湿度 < 20% 自动浇水 | `"自动浇水"` |
| 巴法云远程控制 | 手机上点浇水按钮 | `"远程浇水"` |

所有动作都走 `PlantMonitor::Water(label)` / `PlantMonitor::Light(label)` 同一入口，日志来源不同仅此而已。

---

## 3. 新增模块详解

### 3.1 PlantMonitor（`main/plant_monitor.h/.cc`）

虚拟植物数据引擎，是整个养护系统的"大脑"。**不依赖任何实际硬件**，纯数学模型，可直接替换数据源接口接入真实传感器。

#### 核心接口

| 接口 | 说明 |
|---|---|
| `Initialize()` | 启动时调用，生成初始虚拟土壤湿度 & 光照 |
| `Tick()` | **主循环每秒调用一次**，推进浇水/补光动画、自然衰减、自动养护检查，返回事件位掩码 |
| `Water(label)` | 开始浇水，`label` 区分触发源 |
| `Light(label)` | 开始补光 |
| `SetAutoCare(bool)` | 开启/关闭自动养护 |
| `SetSoilMoisture(int)` / `SetLightLevel(int)` | **演示用**：手动调到低值方便演示自动养护 |
| `GetHistory()` | 最近 20 条养护日志的 JSON（环形缓冲） |
| `GetAutoStatusText()` | 自动养护触发后 10 秒内返回提示文本，看板状态行显示 |

#### Tick() 事件位掩码

| 位 | 宏 | 含义 |
|---|---|---|
| 0x01 | `PLANT_EVENT_WATERING_DONE` | 本次浇水刚结束 |
| 0x02 | `PLANT_EVENT_LIGHTING_DONE` | 本次补光刚结束 |
| 0x04 | `PLANT_EVENT_AUTO_WATER` | 自动养护触发了浇水 |
| 0x08 | `PLANT_EVENT_AUTO_LIGHT` | 自动养护触发了补光 |

#### 自动养护阈值（防振荡设计）

| 指标 | 阈值 | 动作 | 防抖 |
|---|---|---|---|
| 土壤湿度 | < 20% | 自动浇水 5 秒 | 30 秒冷却 |
| 光照强度 | < 15% | 自动补光 10 秒 | 30 秒冷却 |
| 补光后光照抬升 | 基准 + 40% | 防止补光刚结束就又掉回阈值以下 | — |

#### 环形养护日志（20 条定长结构）

```cpp
struct PlantLogEntry {
    time_t ts;                // 事件时刻（服务器时间未同步时为 0）
    unsigned int uptime_min;  // 开机分钟数（时间未同步时的标签）
    char event[48];           // 事件描述（定长避免堆碎片）
};
```

`GetHistory()` 返回 JSON：`{"count":2,"events":[{"time":"14:32","event":"浇水完成(土壤湿度28%)"}]}`。

---

### 3.2 BemfaCloud（`main/bemfa_cloud.h/.cc`）

巴法云 TCP 创客云客户端，与 MQTT 不同，巴法云走**原生 TCP 协议**，更轻量。

#### 协议概要

| 项 | 值 |
|---|---|
| 服务器 | `bemfa.com:8344` |
| 用户私钥 | `d5b46c5865ba445ba3c81feb509d84c4` |
| 订阅主题 | `status`（回显过滤）、`water`、`hLWUuPJ4m002` |
| 心跳 | 60 秒 |
| 状态上报周期 | 30 秒 |
| 指令解析 | TCP 流按 `\n` 逐行拆分 |

#### 指令码映射

| 消息内容 | 动作 | 备注 |
|---|---|---|
| `"1"` | 立即上报状态到 status 主题 | — |
| `"2"` | 触发远程浇水 | 关键词 `"浇水"` / `"water"` 兼容 |
| `"3"` | 触发远程补光 | 关键词 `"补光"` / `"light"` 兼容 |

#### 回显防护（三层）

巴法云会把设备自己发布的消息原封不动回显回来，如果状态文本里恰好包含"浇水"二字就会误判为指令。防护手段：

1. **HandleLine 标记过滤**：所有设备发布的状态消息都包含固定前缀 `"土壤湿度:"`，收到消息先检查此标记，有则直接丢弃
2. **主题隔离**：状态上报走 `status` 主题（发布-only），控制指令走 `water` / `hLWUuPJ4m002`（订阅-only）
3. **TCP 流按行拆分**：确保一条完整消息才解析，避免粘包造成关键词误匹配

#### ⚠️ 启动顺序硬约束

`BemfaCloud::Start()` 必须在 `board.StartNetwork()` **之后**调用。否则：
- `getaddrinfo` 触发 lwIP `tcpip_send_msg_wait_sem (Invalid mbox)` 断言
- 设备进入无限重启循环

**根因**：lwIP 的 tcpip 线程只有在 `esp_netif` 协议栈初始化完成后才存在。

---

### 3.3 Dht11Sensor & led_ctrl

这两个是简单配套模块：

- **Dht11Sensor**：生成虚拟温湿度（温度 20~30℃，湿度 40~80%），对外提供 MCP 工具 `self.dht11.get_temperature` / `self.dht11.get_humidity` / `self.dht11.get_temperature_and_humidity`
- **led_ctrl**：GPIO10 直接操作，浇水时灭灯、补光时亮灯，状态指示用

---

## 4. MCP 工具注册表（7 个绿植专属）

注册位置：`main/mcp_server.cc`（与原小智的音量/灯光/GPIO 工具同处）。

| 工具名 | 输入参数 | 输出 | 作用 |
|---|---|---|---|
| `self.plant.water` | 可选：`seconds`（浇水时长） | `"浇水开始"` | 远程触发浇水 |
| `self.plant.light` | 可选：`seconds`（补光时长） | `"补光开始"` | 远程触发补光 |
| `self.plant.get_status` | — | JSON：`{soil_moisture, light_level, temp, humidity, auto_care}` | 一键查询植物状态 |
| `self.plant.get_history` | — | JSON：环形养护日志 | 语音查询"今天浇过几次水" |
| `self.plant.set_auto_care` | `enabled: bool` | `"自动养护已开启/关闭"` | AI 自动养护管理 |
| `self.plant.set_demo_values` | 可选：`soil` / `light` | `"演示值已设置"` | 把土壤/光照调到低值方便演示 |
| `self.plant.check_health` | — | `"健康良好"` / `"需要浇水"` / `"需要补光"` | AI 综合诊断 |

所有 MCP 工具最终走 `Application::Schedule()` 投递到主循环执行，不直接操作 UI。

---

## 5. 巴法云远程控制协议（原始格式）

TCP 连接 `bemfa.com:8344` 后，按以下格式收发：

**订阅主题**：
```
cmd=1&uid=<USER_KEY>&topic=<TOPIC_LIST>\n
```

**发布消息**：
```
cmd=2&uid=<USER_KEY>&topic=<TOPIC>&msg=<MESSAGE>\n
```

**心跳**：
```
cmd=0&uid=<USER_KEY>\n
```

**下行消息**（巴法云推送）：
```
cmd=3&topic=<TOPIC>&msg=<MESSAGE>\n
```

---

## 6. LVGL 双界面设计

### 看板界面（深绿主题）

```
┌─────────────────────────┐
│    🌿 植语 - 养护看板    │
├──────────┬──────────────┤
│ 土壤湿度  │   光照强度    │
│  45% 💧   │   78% ☀️     │
├──────────┼──────────────┤
│ 温度 / 湿度 │  养护状态    │
│  25.5℃    │  循环滚动...   │
│  65.0% 💨  │  （浇水中/补光）│
└──────────┴──────────────┘
       ↑ 全屏触摸点击切换到聊天界面
```

### 状态文本循环滚动

```cpp
lv_obj_set_style_text_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR, 0);
```

用 `SCROLL_CIRCULAR` 而非 `SCROLL_LINE`，确保养护状态（浇水中/补光中/自动养护提示）在屏幕上**缓慢无限滚动**，不会一闪而过。

### 界面互切

- 触摸全屏点击：看板 ↔ 聊天
- 语音唤醒词触发：强制切到聊天界面
- 主循环每帧根据当前状态调用 `Display::UpdatePlantDashboard()` 刷新看板数据

---

## 7. LVGL 多线程安全规范

小智框架基于 `esp_lvgl_port`，已内置**递归互斥锁**（`xSemaphoreCreateRecursiveMutex`）。

### 规则

| 场景 | 做法 |
|---|---|
| 主循环内（UI 事件回调） | 直接操作 LVGL，无需加锁 |
| FreeRTOS 其他任务（巴法云 TCP / MCP 工具回调） | **禁止直接调用 LVGL API**，必须走 `Application::Schedule(std::function<void()>)` 投递到主循环执行 |
| 投递函数内部 | 必须先调用 `DisplayLockGuard` 加锁后才能操作 LVGL 对象 |

### 错误示范

```cpp
// ❌ 错：巴法云 TCP 任务直接调用 LVGL
void BemfaCloud::HandleRemoteWater() {
    lv_label_set_text(some_label, "正在浇水");  // 多线程竞争 → 崩溃
}
```

### 正确示范

```cpp
// ✅ 对：投递到主循环 + 加锁
void BemfaCloud::HandleRemoteWater() {
    Application::GetInstance().Schedule([] {
        DisplayLockGuard lock;
        lv_label_set_text(some_label, "正在浇水");
    });
}
```

---

## 8. 踩坑与解决方案

### 坑 1：lwIP `Invalid mbox` 断言重启循环

| 项 | 内容 |
|---|---|
| **现象** | 巴法云任务一启动，设备立即触发 `ASSERT (mbox != NULL) @ tcpip_send_msg_wait_sem`，无限重启 |
| **根因** | 任务在 `Board::StartNetwork()` 之前启动，此时 `esp_netif` 协议栈尚未初始化，lwIP tcpip 线程不存在 |
| **修复** | 把 `BemfaCloud::Start()` 从 `main.cc` 挪到 `Application::Run()` 的 `board.StartNetwork()` 调用之后 |
| **复现路径** | 注释掉 `StartNetwork()` → 在主循环前调用 `BemfaCloud().Start()` → 必现 |

### 坑 2：巴法云回显导致循环浇水

| 项 | 内容 |
|---|---|
| **现象** | 每 30 秒自动浇一次水，土壤湿度正常、未开自动养护、也没收到远程指令 |
| **根因** | 设备发布到 `status` 主题的状态文本（例如 `"土壤湿度:45% 浇水:停止"`）被巴法云原封不动回显回来，`HandleLine()` 的关键词匹配扫到"浇水"二字就触发动作 |
| **修复** | 三层防护（见 3.2 节"回显防护"） |

### 坑 3：`%f` printf 在 Nano 格式化下无效

| 项 | 内容 |
|---|---|
| **现象** | 看板上温度/湿度显示为 `0.00`，实际数值正常 |
| **根因** | `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` 关闭了 `%f` 支持，printf 家族一律返回零 |
| **修复** | 用 `std::ostringstream` + `std::fixed` + `std::setprecision(1)` 转浮点字符串 |
| **反例** | `sprintf(buf, "%.1f", temperature);` → 输出 `"0.0"` |
| **正例** | `std::ostringstream oss; oss << std::fixed << std::setprecision(1) << temperature;` |

### 坑 4：头文件嵌套 `/* */` 注释触发 `-Werror=comment`

| 项 | 内容 |
|---|---|
| **现象** | 示例代码复制过来的 plant_monitor.h 编译报错 `comment` |
| **根因** | 工程开了 `-Werror`，嵌套 `/* ... /* ... */ ... */` 被视为非法注释嵌套 |
| **修复** | 把内层 `/* */` 改成 `{ // }` 或直接删除 |

### 坑 5：`std::ostringstream` 构造参数传 `std::fixed` 编译失败

| 项 | 内容 |
|---|---|
| **现象** | `std::ostringstream oss(std::fixed);` 编译报 no matching constructor |
| **根因** | `std::fixed` 是流操纵符（`ios_base&` 返回值），不是构造参数 |
| **修复** | 先构造流，再用 `<< std::fixed` |
| **正例** | `std::ostringstream oss; oss << std::fixed << std::setprecision(1);` |

---

## 9. 构建与烧录（Windows）

```powershell
# 1. 设置 IDF 环境（v5.5.3 路径，根据实际安装调整）
$env:IDF_TOOLS_PATH='D:\ESP32\EspressifV5.5.3\tools'
& 'D:\ESP32\EspressifV5.5.3\frameworks\esp-idf-v5.5.3\export.ps1'

# 2. 首次构建（联网拉取 managed_components，约 3-5 分钟）
idf.py build

# 3. 烧录 + 串口监控（COM14 根据实际端口调整）
idf.py -p COM14 flash monitor

# 4. 擦除 Flash（更换固件或测试 NVS 持久化时用）
idf.py -p COM14 erase-flash
```

**编译产物**：`build/xiaozhi.bin` 约 4MB，退出码 0。

### 必选 menuconfig 项

| 项 | 值 | 原因 |
|---|---|---|
| `Component config → ESP PSRAM` | 启用 | 摄像头帧缓冲依赖 |
| `Component config → Newlib` | 启用 `Nano formatting` | 减小固件体积（注意坑 3 的 `%f`） |
| `Component config → Driver → Camera` | GC0308 + PCA9557 | 粤嵌板卡预设 |
| `Board type` | gec-esp32s3-board | — |

---

## 10. 后续扩展方向

| 方向 | 难度 | 说明 |
|---|---|---|
| 真实传感器接入 | 低 | 电容土壤湿度 ADC + BH1750 I²C，替换 `PlantMonitor` 内部数据源接口，上层调用不变 |
| MJPEG 摄像头服务器 | 中 | HTTP 服务器循环 `esp_camera_fb_get()` 推流，约 50 行代码 |
| MCP 摄像头 AI 健康诊断 | 中 | 已有拍照能力，缺的是 JPEG 上传服务器 + AI 反馈解析 |
| OTA 远程升级 | 低 | 小智框架自带 OTA，只需在板卡 config.json 里打开 |
| 提交规范 | 低 | 建议按 screenshot 方案 A：每次提交注明 `feat: 新增绿植养护闭环` / `fix: 巴法云回显循环浇水` 等，方便面试官追溯 |

---

## 附：原始文档索引

小智框架自带文档（未改动，保留在原位置）：

- `docs/websocket.md` — WebSocket 通信协议
- `docs/mqtt-udp.md` — MQTT + UDP 混合协议
- `docs/mcp-protocol.md` — MCP 协议交互流程
- `docs/mcp-usage.md` — MCP 物联网控制用法说明

本文件 `docs/plantalk/README.md` 是植语专属的技术整合文档，所有与绿植养护、巴法云、自动养护、双界面 UI 相关的内容都在此处。
