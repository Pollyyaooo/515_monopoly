# Monopoly Physical Game System — 系统架构文档

> **版本:** v1.0 · **日期:** 2026-05-08  
> **项目:** UW MSTI 515 · Phygital Monopoly

---

## 1. 系统总览

本系统将经典大富翁拆分为两类设备，职责清晰：

| 设备 | 硬件 | 职责 |
|------|------|------|
| **Central Display（公共屏幕）** | CrowPanel ESP32-S3 5" (DIS02050A) | 讲故事、投骰子动画、公共事件、回合广播 |
| **Player Device（玩家设备）** | Seeed XIAO ESP32S3 + SSD1351 128×128 OLED + PN532 | 个人资产、个人决策、支付确认、NFC 位置扫描 |

通信方式：**Bluetooth Low Energy (BLE)**  
拓扑：Central Display 作为 **BLE Central（Server）**，最多 4 台 Player Device 作为 **BLE Peripherals（Clients）**

```
┌──────────────────────────────────────────────────────────────────┐
│                     CrowPanel 5" (Central Display)               │
│  ESP32-S3 WROOM-1 · 800×480 Touch · Buzzer · Speaker            │
│                                                                  │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────────────┐  │
│  │ Game Engine │  │  BLE Central │  │   Display / Animation  │  │
│  │  (Master)   │←→│  (4 slots)   │  │  Dice · Cards · Events │  │
│  └─────────────┘  └──────┬───────┘  └────────────────────────┘  │
│                           │ BLE                                   │
└───────────────────────────┼──────────────────────────────────────┘
                            │
         ┌──────────────────┼──────────────────┐
         │                  │                  │
  ┌──────┴──────┐   ┌──────┴──────┐   ┌──────┴──────┐
  │  Player 1   │   │  Player 2   │   │  Player 3/4 │
  │ XIAO ESP32S3│   │ XIAO ESP32S3│   │ XIAO ESP32S3│
  │ OLED + NFC  │   │ OLED + NFC  │   │ OLED + NFC  │
  └─────────────┘   └─────────────┘   └─────────────┘
```

---

## 2. 硬件规格

### 2.1 Central Display — CrowPanel ESP32-S3 5"

| 参数 | 值 |
|------|----|
| SKU | DIS02050A V1.1 |
| MCU | ESP32-S3 WROOM-1 |
| 屏幕 | 5.0" TFT，800×480，电容触摸 |
| 接口 | UART0/1-IN/OUT · I2C-OUT · USB-5V-IN |
| 音频 | 板载 Buzzer + Speaker (SPK 接口) |
| 存储 | TF Card slot |
| 电源 | USB-5V-IN / BAT |
| BLE | ESP32-S3 内置 BLE 5.0 |

**推荐开发框架：** Arduino (PlatformIO) + LVGL 8.x（UI 动画）

### 2.2 Player Device — Seeed XIAO ESP32S3

| 参数 | 值 |
|------|----|
| MCU | ESP32-S3 |
| 显示 | SSD1351 128×128 RGB OLED (SPI) |
| NFC | PN532（I2C 模式） |
| 输入 | 5× 触觉按钮（UP / DOWN / LEFT / RIGHT / OK） |
| BLE | ESP32-S3 内置 BLE 5.0 |

---

## 3. BLE 通信协议

### 3.1 拓扑与连接

- Central Display 开启 **BLE Server**，广播服务 UUID。
- 每台 Player Device 启动后扫描并连接 Central，注册自己的 Player ID（0~3）。
- 最多同时 4 条 BLE 连接。

### 3.2 GATT 服务定义

```
Service UUID: 0xAA01  (Monopoly Game Service)
│
├── Characteristic 0xBB01  [NOTIFY | WRITE]
│   名称: Game Event (Central → Players)
│   方向: Central 广播给所有 Player
│   载荷: JSON 或固定字节包（见 3.3）
│
├── Characteristic 0xBB02  [WRITE]
│   名称: Player Action (Player → Central)
│   方向: Player Device 上报动作
│   载荷: 固定字节包
│
└── Characteristic 0xBB03  [READ]
    名称: Game State Snapshot
    方向: Player 可随时读取完整游戏状态
    载荷: 压缩状态包
```

### 3.3 消息包格式

#### Central → Player（Game Event Packet）

```
[1B] msg_type  | 消息类型（见下表）
[1B] player_id | 目标 player（0xFF = 广播全体）
[2B] payload_a | 主要参数
[2B] payload_b | 次要参数
[1B] checksum  | XOR 校验
```

| msg_type | 含义 | payload_a | payload_b |
|----------|------|-----------|-----------|
| `0x01` | YOUR_TURN | player_id | — |
| `0x02` | DICE_RESULT | die1 (1-6) | die2 (1-6) |
| `0x03` | MOVE_TO | square_index | — |
| `0x04` | CHANCE_CARD | card_id | — |
| `0x05` | COMMUNITY_CARD | card_id | — |
| `0x06` | PAY_REQUEST | amount (uint16) | recipient_id |
| `0x07` | COLLECT | amount (uint16) | source (0=bank) |
| `0x08` | GO_TO_JAIL | — | — |
| `0x09` | GAME_OVER | winner_id | — |
| `0x0A` | SYNC_BALANCE | balance (uint16) | — |
| `0x0B` | PROPERTY_UPDATE | square_index | owner_id |

#### Player → Central（Player Action Packet）

```
[1B] msg_type  | 动作类型
[1B] player_id | 发送方
[2B] payload_a |
[2B] payload_b |
[1B] checksum  |
```

| msg_type | 含义 | payload_a | payload_b |
|----------|------|-----------|-----------|
| `0x81` | ROLL_DICE | — | — |
| `0x82` | BUY_PROPERTY | square_index | — |
| `0x83` | PAY_CONFIRM | amount | recipient_id |
| `0x84` | PAY_REJECT | — | — |
| `0x85` | USE_CARD | card_id | — |
| `0x86` | NFC_SCANNED | square_index | — |
| `0x87` | END_TURN | — | — |
| `0x88` | MORTGAGE | square_index | — |

---

## 4. Game Engine（运行在 Central Display）

Central Display 是游戏的**唯一权威状态机**，所有 Player Device 只展示结果、等待指令。

### 4.1 状态机

```
WAITING_FOR_PLAYERS
      │ 4 players connected
      ▼
   GAME_START
      │ broadcast turn order
      ▼
┌─► PLAYER_TURN_START
│     │ broadcast YOUR_TURN → player_id
│     ▼
│   WAITING_ROLL
│     │ receive ROLL_DICE from current player
│     ▼
│   ROLLING_DICE ──── 播放骰子动画（Central屏幕）
│     │ animation done, compute result
│     │ broadcast DICE_RESULT
│     ▼
│   MOVING_PLAYER ─── 广播 MOVE_TO
│     │
│     ▼
│   RESOLVE_SQUARE ───────────────────────────────────────────────┐
│     │ SQ_PROPERTY  → 广播 PAY_REQUEST 或 BUY 选项              │
│     │ SQ_CHANCE    → 抽卡，广播 CHANCE_CARD + 执行效果         │
│     │ SQ_COMMUNITY → 抽卡，广播 COMMUNITY_CARD + 执行效果      │
│     │ SQ_TAX       → 广播 PAY_REQUEST (bank)                   │
│     │ SQ_GO_JAIL   → 广播 GO_TO_JAIL                           │
│     │ SQ_SPECIAL   → 无动作 / Collect $200                     │
│     │                                                           │
│     ▼                                                           │
│   WAITING_PLAYER_ACTION ←──────────────────────────────────────┘
│     │ receive PAY_CONFIRM / BUY / END_TURN
│     │ update GameState
│     │ broadcast SYNC_BALANCE to affected players
│     ▼
│   CHECK_BANKRUPTCY ─── 检查是否有玩家破产
│     │ if bankrupt → remove player
│     │ if 1 player left → GAME_OVER
│     ▼
└─── TURN_END ─── next player
```

### 4.2 GameState 数据结构（Central 持有）

```cpp
struct GameState {
  Player  players[4];       // 所有玩家状态
  Square  board[40];        // 完整 40 格棋盘
  int     currentTurnIdx;   // 当前回合玩家
  int     totalPlayers;     // 在线玩家数
  int     chanceDeck[16];   // Chance 牌堆
  int     communityDeck[16];// Community Chest 牌堆
  int     chanceTop;        // 当前抽牌指针
  int     communityTop;
  bool    doublesRolled;    // 是否连续骰到 doubles
  int     doublesCount;
};
```

---

## 5. Central Display — UI 模块

### 5.1 屏幕页面划分

```
┌─────────────────────────────────────────┐
│  CrowPanel 5"  800×480                  │
│                                         │
│  主要页面（全屏）：                      │
│  ┌──────────────────────────────────┐   │
│  │ IDLE / WAITING    等待玩家连接   │   │
│  ├──────────────────────────────────┤   │
│  │ DICE ROLL SCREEN  投骰子动画     │   │
│  ├──────────────────────────────────┤   │
│  │ EVENT SCREEN      公共事件通知   │   │
│  ├──────────────────────────────────┤   │
│  │ CARD DRAW         Chance/Community│  │
│  ├──────────────────────────────────┤   │
│  │ BOARD OVERVIEW    棋盘全览（可选）│  │
│  └──────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

### 5.2 骰子动画（Dice Roll Screen）

- 收到 `ROLL_DICE` 请求后触发
- 播放 **随机旋转骰子精灵图动画**（LVGL `lv_animimg` 或帧循环）
- 动画时长：1.5~2 秒，渐慢停止
- 停止后显示最终骰子面值（大字，带声效 Buzzer）
- 发送 `DICE_RESULT` BLE 广播

**实现方案：**
```
TF Card 存储骰子帧图片（BMP/PNG）
→ LVGL lv_img / lv_animimg 逐帧播放
→ 结束帧 = 实际 RNG 结果
```

### 5.3 公共事件屏（Event Screen）

显示触发事件，保持 3~5 秒后自动进入下一状态：

| 事件 | 显示内容 |
|------|---------|
| 回合开始 | "🎲 Alice's Turn!" + 玩家颜色背景 |
| 移动完成 | "Alice → Park Place" |
| 抽 Chance 牌 | 大字显示牌面内容 + 图标 |
| 抽 Community 牌 | 大字显示牌面内容 + 图标 |
| 支付发生 | "Alice pays Bob $50" |
| 入狱 | "🔒 Alice goes to Jail!" |
| 通过 Go | "Alice collects $200!" |
| 破产 | "Dave is bankrupt! 💸" |
| 游戏结束 | "Alice WINS! 🏆" |

### 5.4 抽卡画面（Card Draw Screen）

- 动画：卡片从牌堆飞出，翻转展示正面
- 正面显示：卡牌名称 + 描述文字 + 效果图标
- 保持显示直到触摸屏或收到 `END_TURN`

---

## 6. Player Device — UI 模块（已有基础版）

### 6.1 现有页面（保持不变）

| 页面 | 内容 |
|------|------|
| PAGE_HOME | 玩家名、余额、位置、道具/卡牌数量 |
| PAGE_ACTION | 当前格子可执行动作（买/付租金/税） |
| PAGE_PROPERTIES | 持有地产列表（含房屋图标） |
| PAGE_CARDS | 持有卡牌列表 |
| PAGE_NFC_SCAN | NFC 扫描提示 |

### 6.2 新增页面（BLE 接入后）

| 页面 | 触发时机 | 内容 |
|------|---------|------|
| PAGE_WAIT_TURN | 非当前玩家 | "Bob's turn..." + 当前回合进度 |
| PAGE_PAY_CONFIRM | 收到 `PAY_REQUEST` | "Pay $50 to Bob? OK/Cancel" |
| PAGE_CARD_RECEIVED | 收到 Chance/Community 结果 | 显示抽到的牌效果 |
| PAGE_BANKRUPT | 余额 < 0 | "You are bankrupt!" |

### 6.3 BLE 接入流程（Player Device 侧）

```
setup():
  1. 扫描 BLE，连接 UUID=0xAA01 的 Central
  2. 订阅 Characteristic 0xBB01 (NOTIFY)
  3. 发送注册包：player_id = 用户选择的颜色/编号

loop():
  - 监听 BLE NOTIFY 回调 → 解析 Game Event → 更新本地状态 → 刷新 OLED
  - 处理按钮 → 构造 Player Action 包 → BLE WRITE 到 0xBB02
  - NFC 扫描 → 发送 NFC_SCANNED 包 → 等待 Central 下发 MOVE_TO
```

---

## 7. 开发分工与文件结构

```
monopoly_final/
│
├── central_display/              # CrowPanel 5" 固件
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp              # setup / loop
│       ├── game_engine.h/cpp     # GameState + 状态机
│       ├── ble_central.h/cpp     # BLE Server / 4连接管理
│       ├── ui_dice.h/cpp         # 骰子动画
│       ├── ui_event.h/cpp        # 公共事件屏
│       ├── ui_card.h/cpp         # 抽卡画面
│       └── cards_data.h          # 所有 Chance / Community 牌内容
│
├── player_device/                # XIAO ESP32S3 固件（含现有代码）
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp              # setup / loop（扩展现有版本）
│       ├── ble_client.h/cpp      # BLE Peripheral / NOTIFY 处理
│       ├── game_state.h/cpp      # 本地 GameState 镜像
│       ├── ui_pages.h/cpp        # 所有页面绘制（现有 + 新增）
│       └── nfc_handler.h/cpp     # PN532 扫描逻辑
│
├── shared/                       # 两端共用定义
│   └── protocol.h                # BLE UUID / 消息包结构 / 枚举
│
└── SYSTEM_ARCHITECTURE.md        # 本文档
```

---

## 8. 开发阶段规划

### Phase 1：BLE 连接基础（无游戏逻辑）
- [ ] Central：BLE Server 广播 + 接受 4 连接
- [ ] Player：BLE Client 扫描连接 + 发送心跳
- [ ] 验证：4 台设备同时连接，收发测试包

### Phase 2：骰子 + 回合广播
- [ ] Central：骰子动画（LVGL）+ RNG + 广播 DICE_RESULT
- [ ] Central：YOUR_TURN 广播 + 事件屏显示
- [ ] Player：接收 YOUR_TURN → 高亮显示 → 按 OK 发送 ROLL_DICE

### Phase 3：NFC 位置 + 格子解析
- [ ] Player：PN532 扫描 → 发送 NFC_SCANNED
- [ ] Central：接收 NFC_SCANNED → 更新 position → 广播 MOVE_TO + 解析格子事件

### Phase 4：支付 + 资产管理
- [ ] Central：PAY_REQUEST 定向发送 + 等待 PAY_CONFIRM
- [ ] Player：PAY_CONFIRM 页面 + BLE 回复
- [ ] Central：SYNC_BALANCE 广播 → Player 更新余额显示

### Phase 5：Chance / Community 牌
- [ ] Central：抽卡动画 + 牌效果执行 + 广播
- [ ] Player：接收 CARD_RECEIVED → 显示卡牌内容页面

### Phase 6：整体测试 + 音效
- [ ] CrowPanel Buzzer：骰子 / 收钱 / 入狱 音效
- [ ] 端到端完整游戏流程测试

---

## 9. 关键技术选型说明

| 决策 | 选择 | 原因 |
|------|------|------|
| 通信协议 | BLE（非 WiFi） | 无需路由器，现场部署简单；ESP32-S3 原生支持 |
| Central 为 Server | 是 | 状态权威在 Central，避免多端同步冲突 |
| UI 框架（Central）| LVGL 8.x | CrowPanel 官方支持；动画能力强（animimg、过渡效果） |
| UI 框架（Player）| Adafruit GFX | 已有稳定基础代码，资源占用低 |
| NFC → 位置映射 | Player 本地查表 + 上报 index | 减少 BLE 传输量；Central 收到 index 即可 |
| 游戏状态存储 | Central 单一权威 | Player Device 只保存本地镜像，以 Central 为准 |

---

## 10. BLE 实现参考（Arduino NimBLE）

两端均使用 **NimBLE-Arduino** 库（PlatformIO 依赖：`h2zero/NimBLE-Arduino`），资源占用比官方 ESP-IDF BLE 栈小约 50%。

```cpp
// platformio.ini 公共依赖
[env]
lib_deps =
  h2zero/NimBLE-Arduino @ ^1.4.1
  lvgl/lvgl @ ^8.3.0          ; Central only
  adafruit/Adafruit GFX Library
  adafruit/Adafruit SSD1351 library
  adafruit/Adafruit PN532     ; Player only
```

---

*文档由 Claude Code 辅助生成，基于现有 player_device_test 代码与 CrowPanel DIS02050A 硬件规格。*
