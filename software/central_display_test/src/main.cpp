// =============================================================
// central_display_test / main.cpp
// CrowPanel ESP32-S3 5.0" (DIS02050A) V1.1
// 引脚来源: 官方 GitHub factory_sourcecode/V1.1/HMI-bigInch5/
//
// 测试内容:
//   1. 颜色全屏填充（红/绿/蓝）验证 RGB 接口
//   2. 文字渲染
//   3. 触摸坐标读取（Serial 打印）
//   4. 点 ROLL DICE 按钮 → 骰子动画 + 结果
//   5. Buzzer 短响（通过 STC8H1K28 I2C 控制）
// =============================================================

// ─────────────────────────────────────────────────────────────
// [1] LovyanGFX — 完全来自官方 LovyanGFX_Driver.h
// ─────────────────────────────────────────────────────────────
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Wire.h>

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB    _bus_instance;
  lgfx::Panel_RGB  _panel_instance;
  lgfx::Touch_GT911 _touch_instance;
  // 注意: 无 Light_PWM — 背光由 STC8H1K28 I2C 控制

  LGFX(void) {
    // ── Panel 尺寸 ──────────────────────────────────────────
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width  = 800;
      cfg.memory_height = 480;
      cfg.panel_width   = 800;
      cfg.panel_height  = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }

    // ── PSRAM ───────────────────────────────────────────────
    {
      auto cfg = _panel_instance.config_detail();
      cfg.use_psram = 2;   // 双缓冲：DMA读前台，CPU写后台，endWrite()原子切换，消除花屏
      _panel_instance.config_detail(cfg);
    }

    // ── RGB Bus（16-bit 并行）— 来自官方 V1.1 驱动 ─────────
    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;

      // D0–D15（16-bit RGB565 数据线）
      cfg.pin_d0  = GPIO_NUM_21;   // B[4] MSB of Blue
      cfg.pin_d1  = GPIO_NUM_47;   // B[3]
      cfg.pin_d2  = GPIO_NUM_48;   // B[2]
      cfg.pin_d3  = GPIO_NUM_45;   // B[1]
      cfg.pin_d4  = GPIO_NUM_38;   // B[0] LSB of Blue
      cfg.pin_d5  = GPIO_NUM_9;    // G[5] MSB of Green
      cfg.pin_d6  = GPIO_NUM_10;   // G[4]
      cfg.pin_d7  = GPIO_NUM_11;   // G[3]
      cfg.pin_d8  = GPIO_NUM_12;   // G[2]
      cfg.pin_d9  = GPIO_NUM_13;   // G[1]
      cfg.pin_d10 = GPIO_NUM_14;   // G[0] LSB of Green
      cfg.pin_d11 = GPIO_NUM_7;    // R[4] MSB of Red
      cfg.pin_d12 = GPIO_NUM_17;   // R[3]
      cfg.pin_d13 = GPIO_NUM_18;   // R[2]
      cfg.pin_d14 = GPIO_NUM_3;    // R[1]
      cfg.pin_d15 = GPIO_NUM_46;   // R[0] LSB of Red

      // 同步信号
      cfg.pin_henable = GPIO_NUM_42;  // DE
      cfg.pin_vsync   = GPIO_NUM_41;
      cfg.pin_hsync   = GPIO_NUM_40;
      cfg.pin_pclk    = GPIO_NUM_39;
      cfg.freq_write  = 21000000;     // 21 MHz

      // 时序（来自官方）
      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch  = 8;
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch  = 8;
      cfg.pclk_idle_high    = 1;   // ← 重要！官方配置为 1

      _bus_instance.config(cfg);
    }
    _panel_instance.setBus(&_bus_instance);

    // ── Touch GT911 ─────────────────────────────────────────
    {
      auto cfg = _touch_instance.config();
      cfg.x_min    = 0;
      cfg.x_max    = 800;
      cfg.y_min    = 0;
      cfg.y_max    = 480;
      cfg.pin_int  = -1;
      cfg.pin_rst  = -1;
      cfg.bus_shared      = true;   // Wire 已管理 I2C_NUM_0，让 LovyanGFX 共用，不重复安装驱动
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.pin_sda  = GPIO_NUM_15;
      cfg.pin_scl  = GPIO_NUM_16;
      cfg.freq     = 400000;
      cfg.i2c_addr = 0x5D;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};

static LGFX display;

// ─────────────────────────────────────────────────────────────
// [2] STC8H1K28 控制（背光 + Buzzer）
//     I2C addr: 0x30，总线: SDA=IO15, SCL=IO16
//     官方协议：单次 I2C 事务，直接发命令字节
//       0x10 = 亮屏（标准亮度）
//       0x19 = STC 复位/初始化（启动等待时使用）
//       0x15 = Buzzer ON，0x16 = Buzzer OFF
// ─────────────────────────────────────────────────────────────
#define STC_ADDR      0x30
#define STC_BL_ON     0x10   // 亮屏命令（官方确认）
#define STC_INIT      0x19   // STC 启动等待时发送
#define STC_BZ_ON     0x15   // Buzzer ON（官方 Desktop_Assistant V1.1 确认）
#define STC_BZ_OFF    0x16   // Buzzer OFF（同上）

void stcSend(uint8_t cmd) {
  Wire.beginTransmission(STC_ADDR);
  Wire.write(cmd);
  Wire.endTransmission();
}

// 等待 STC8H1K28(0x30) 和 GT911(0x5D) 都就绪，与官方逻辑一致
void waitForI2CDevices() {
  Serial0.println("[I2C] Waiting for 0x30 (STC) and 0x5D (GT911)...");
  while (1) {
    Wire.beginTransmission(0x30);
    bool stcOk = (Wire.endTransmission() == 0);
    Wire.beginTransmission(0x5D);
    bool gtOk  = (Wire.endTransmission() == 0);

    if (stcOk && gtOk) {
      Serial0.println("[I2C] Both devices ready.");
      break;
    }
    Serial0.printf("[I2C] Not ready yet (STC=%d GT911=%d), retrying...\n", stcOk, gtOk);
    // 官方：发 STC 复位命令 + 拉低 GPIO1（GT911 复位线）
    stcSend(STC_INIT);
    pinMode(1, OUTPUT);
    digitalWrite(1, LOW);
    delay(120);
    pinMode(1, INPUT);
    delay(100);
  }
}

void beep(int ms = 80) {
  // 官方 Alarm_Clock.cpp 确认：蜂鸣器与背光完全独立，BZ_OFF 后不需要重发 BL_ON
  stcSend(STC_BZ_ON);
  delay(ms);
  stcSend(STC_BZ_OFF);
}

// ─────────────────────────────────────────────────────────────
// [3] COLOR CONSTANTS
// ─────────────────────────────────────────────────────────────
#define C_BLACK     0x000000UL
#define C_WHITE     0xFFFFFFUL
#define C_RED       0xFF2020UL
#define C_GREEN     0x20C020UL
#define C_BLUE      0x2060FFUL
#define C_YELLOW    0xFFE000UL
#define C_ORANGE    0xFF8000UL
#define C_DARKGRAY  0x222244UL  // 深蓝色背景（肉眼可见，不会和黑色混淆）
#define C_LIGHTGRAY 0xCCCCCCUL  // 浅灰色文字（在深蓝背景上清晰可见）
#define C_GOLD      0xFFD700UL

#define LCD_W 800
#define LCD_H 480

// ESP32-S3 PSRAM cache flush: DMA 直接读物理 PSRAM，CPU 写的像素先在缓存中
// 每次完整绘制结束后必须 endWrite()+startWrite() 才能让 DMA 看到新内容
inline void flushDisplay() {
  display.endWrite();
  display.startWrite();
}

// ─────────────────────────────────────────────────────────────
// [4] STEP 1 — 色彩扫描（验证 RGB 接口接线）
// ─────────────────────────────────────────────────────────────
void colorSweepTest() {
  Serial0.println("[TEST] Color sweep...");
  struct { uint32_t color; const char* name; } colors[] = {
    { C_RED,   "RED"   },
    { C_GREEN, "GREEN" },
    { C_BLUE,  "BLUE"  },
    { C_WHITE, "WHITE" },
    { C_BLACK, "BLACK" },
  };
  for (auto& c : colors) {
    display.fillScreen(c.color);
    display.setTextColor(~c.color);
    display.setTextSize(4);
    display.setCursor(320, 220);
    display.print(c.name);
    flushDisplay();   // flush PSRAM cache so DMA sees the new frame
    Serial0.printf("  -> %s\n", c.name);
    delay(700);
  }
}

// ─────────────────────────────────────────────────────────────
// [5] IDLE 屏幕
// ─────────────────────────────────────────────────────────────
void drawIdleScreen() {
  display.fillScreen(C_DARKGRAY);

  display.setTextColor(C_GOLD);
  display.setTextSize(6);
  display.setCursor(180, 80);
  display.print("MONOPOLY");

  display.setTextColor(C_LIGHTGRAY);
  display.setTextSize(2);
  display.setCursor(260, 180);
  display.print("Central Display  V1.1 Test");

  // ROLL DICE 按钮
  display.fillRoundRect(250, 250, 300, 100, 20, C_BLUE);
  display.setTextColor(C_WHITE);
  display.setTextSize(3);
  display.setCursor(298, 288);
  display.print("ROLL DICE");

  display.setTextColor(C_LIGHTGRAY);
  display.setTextSize(2);
  display.setCursor(270, 420);
  display.print("Tap the button to roll");

  flushDisplay();   // flush so button + bottom text reach PSRAM before DMA reads
  Serial0.println("[UI] Idle screen ready. Waiting for touch...");
}

// ─────────────────────────────────────────────────────────────
// [6] 骰子动画
// ─────────────────────────────────────────────────────────────

// 接受任意 LovyanGFX 派生对象（display 或 Sprite），坐标相对于该对象
void drawDieFace(lgfx::LovyanGFX* gfx, int x, int y, int size, int face,
                 uint32_t bg, uint32_t pip) {
  gfx->fillRoundRect(x, y, size, size, size / 8, bg);
  gfx->drawRoundRect(x, y, size, size, size / 8, C_LIGHTGRAY);

  struct Pip { int c, r; };
  const Pip layout[6][6] = {
    { {1,1} },
    { {0,0},{2,2} },
    { {0,0},{1,1},{2,2} },
    { {0,0},{2,0},{0,2},{2,2} },
    { {0,0},{2,0},{1,1},{0,2},{2,2} },
    { {0,0},{2,0},{0,1},{2,1},{0,2},{2,2} },
  };
  const int counts[6] = {1,2,3,4,5,6};
  int pipR   = size / 10;
  int margin = size / 5;
  int step   = (size - margin*2) / 2;

  for (int i = 0; i < counts[face-1]; i++) {
    int px = x + margin + layout[face-1][i].c * step;
    int py = y + margin + layout[face-1][i].r * step;
    gfx->fillCircle(px, py, pipR, pip);
  }
}

void diceRollAnimation(int &r1, int &r2) {
  beep(40);
  Serial0.println("[DICE] Rolling...");

  const int SZ   = 180;
  const int GAP  = 60;
  const int TX   = (LCD_W - SZ*2 - GAP) / 2;
  const int TY   = (LCD_H - SZ) / 2 - 20;

  // 双缓冲模式下，每次 flushDisplay()=endWrite()+startWrite() 原子切换前后台缓冲区
  // CPU 始终写后台，DMA 始终读前台，无花屏

  // 快速闪烁 → 减速 → 停止
  // 双缓冲关键：每帧必须 fillScreen 重绘整个后台缓冲区
  // 原因：flushDisplay() 切换后，后台变成旧前台（内容是上上帧），
  //       若只 fillRect 局部，剩余区域仍是旧内容，切换后 DMA 读到混合帧 → 花屏
  for (int f = 0; f < 32; f++) {
    display.fillScreen(C_DARKGRAY);        // 重绘整个后台，确保完整帧
    display.setTextColor(C_WHITE);
    display.setTextSize(3);
    display.setCursor(310, 60);
    display.print("Rolling...");
    drawDieFace(&display, TX,         TY, SZ, random(1,7), C_WHITE, C_BLACK);
    drawDieFace(&display, TX+SZ+GAP, TY, SZ, random(1,7), C_WHITE, C_BLACK);
    flushDisplay();   // 原子切换，DMA 看到完整帧

    int d = 25 + f * f * 2;
    delay(d > 280 ? 280 : d);
  }

  // 最终结果（金色）— 同样重绘整个后台，避免双缓冲混帧
  r1 = random(1, 7);
  r2 = random(1, 7);
  display.fillScreen(C_DARKGRAY);
  drawDieFace(&display, TX,         TY, SZ, r1, C_GOLD,  C_BLACK);
  drawDieFace(&display, TX+SZ+GAP, TY, SZ, r2, C_GOLD,  C_BLACK);

  char buf[24];
  snprintf(buf, sizeof(buf), "Total: %d", r1 + r2);
  display.setTextColor(C_WHITE);
  display.setTextSize(4);
  int tw = display.textWidth(buf);
  display.setCursor((LCD_W - tw) / 2, TY + SZ + 20);
  display.print(buf);

  if (r1 == r2) {
    display.setTextColor(C_ORANGE);
    display.setTextSize(3);
    int dw = display.textWidth("DOUBLES!");
    display.setCursor((LCD_W - dw) / 2, TY + SZ + 70);
    display.print("DOUBLES!");
    beep(200);
  } else {
    beep(80);
  }

  display.setTextColor(C_LIGHTGRAY);
  display.setTextSize(2);
  display.setCursor(280, LCD_H - 40);
  display.print("Tap anywhere to roll again");

  flushDisplay();
  Serial0.printf("[DICE] %d + %d = %d%s\n",
    r1, r2, r1+r2, r1==r2 ? " DOUBLES!" : "");
}

// ─────────────────────────────────────────────────────────────
// [7] SETUP / LOOP
// ─────────────────────────────────────────────────────────────
enum AppState { STATE_COLOR_TEST, STATE_IDLE, STATE_ROLLING, STATE_RESULT };
AppState appState = STATE_COLOR_TEST;

void setup() {
  Serial0.begin(115200);   // UART0 (TX=GPIO43, RX=GPIO44) — 与 log_e 同一路
  delay(200);

  Serial0.println("\n=== CrowPanel 5\" V1.1 Display Test ===");
  Serial0.printf("[PSRAM] size=%u  free=%u\n",
    (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  Serial0.flush();

  // 官方要求：GPIO19 设为输出（板载功能引脚）
  pinMode(19, OUTPUT);

  // I2C 初始化
  Wire.begin(15, 16);
  delay(50);

  // ── 阻塞等待 STC8H1K28(0x30) 和 GT911(0x5D) 都就绪 ───────
  // 与官方 V1.1 完全一致，避免背光命令发送时 STC 未启动
  waitForI2CDevices();

  // 背光：官方 BigInch_LVGL.ino 在 display.init() 之前发送 0x10（与官方顺序一致）
  stcSend(STC_BL_ON);
  Serial0.println("[STC] Backlight ON (0x10)");

  // 显示屏初始化 — 必须按 init → initDMA → startWrite 顺序
  // 官方代码始终包含这三步，缺少 initDMA/startWrite 会导致黑屏
  bool ok = display.init();
  Serial0.printf("[LCD] init %s\n", ok ? "OK" : "FAILED");
  display.initDMA();         // ← 建立 DMA framebuffer，RGB 屏幕必须
  display.startWrite();      // ← 开启写入模式，fillScreen 才生效
  display.fillScreen(0x0000);  // 先填黑，确认 DMA 正常

  Serial0.printf("[HEAP] free=%u\n", (unsigned)ESP.getFreeHeap());

  randomSeed((uint32_t)esp_random());

  // Step 1: 色彩扫描
  colorSweepTest();
  Serial0.println("[LCD] Color sweep done. If colors correct, RGB wiring is good.");

  // Step 2: Idle 屏幕
  drawIdleScreen();
  appState = STATE_IDLE;
}

void loop() {
  static bool    wasTouched    = false;
  static uint32_t resultAt     = 0;

  lgfx::touch_point_t tp[1];
  bool touched = display.getTouch(tp, 1) > 0;

  if (touched && !wasTouched) {
    Serial0.printf("[TOUCH] x=%d y=%d\n", tp[0].x, tp[0].y);
  }

  switch (appState) {

    case STATE_IDLE:
      // 按钮绘制区域 (250,250)→(550,350)，检测区域向外扩展 20px 容忍 GT911 偏差
      if (touched && !wasTouched) {
        bool hit = (tp[0].x >= 230 && tp[0].x <= 570 &&
                    tp[0].y >= 230 && tp[0].y <= 370);
        Serial0.printf("[BTN] touch=(%d,%d) %s\n", tp[0].x, tp[0].y, hit ? "HIT" : "MISS");
        if (hit) {
          appState = STATE_ROLLING;
        }
      }
      break;

    case STATE_ROLLING: {
      int r1, r2;
      diceRollAnimation(r1, r2);
      resultAt = millis();
      appState = STATE_RESULT;
      break;
    }

    case STATE_RESULT:
      // 5 秒后或任意触摸 → 回 Idle
      if ((touched && !wasTouched) || millis() - resultAt > 5000) {
        drawIdleScreen();
        appState = STATE_IDLE;
      }
      break;

    default:
      break;
  }

  wasTouched = touched;
  delay(16);
}
