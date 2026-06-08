# CrowPanel Advance 5.0" HMI ESP32-S3 — DIS02050A V1.1 引脚速查

> 来源：官方 GitHub `factory_sourcecode/V1.1/HMI-bigInch5/LovyanGFX_Driver.h`  
> **所有引脚已从官方代码确认，可直接使用**

---

## RGB 显示接口（16-bit 并行，Bus_RGB）

| 信号 | GPIO | 说明 |
|------|------|------|
| D0  | 21  | Blue[4] MSB |
| D1  | 47  | Blue[3] |
| D2  | 48  | Blue[2] |
| D3  | 45  | Blue[1] |
| D4  | 38  | Blue[0] LSB |
| D5  | 9   | Green[5] MSB |
| D6  | 10  | Green[4] |
| D7  | 11  | Green[3] |
| D8  | 12  | Green[2] |
| D9  | 13  | Green[1] |
| D10 | 14  | Green[0] LSB |
| D11 | 7   | Red[4] MSB |
| D12 | 17  | Red[3] |
| D13 | 18  | Red[2] |
| D14 | 3   | Red[1] |
| D15 | 46  | Red[0] LSB |
| DE (henable) | **42** | Data Enable |
| VSYNC | **41** | |
| HSYNC | **40** | |
| PCLK  | **39** | 21 MHz |

### 时序参数（官方值）

```
pclk_idle_high    = 1      ← 重要！不设会花屏
hsync_polarity    = 0
hsync_front_porch = 8
hsync_pulse_width = 4
hsync_back_porch  = 8
vsync_polarity    = 0
vsync_front_porch = 8
vsync_pulse_width = 4
vsync_back_porch  = 8
freq_write        = 21000000
```

---

## 触摸控制器（GT911）

| 参数 | 值 |
|------|----|
| 芯片 | GT911 |
| I2C 地址 | **0x5D** |
| SDA | **IO15** |
| SCL | **IO16** |
| INT | -1（无）|
| RST | -1（无）|
| I2C port | I2C_NUM_0 |
| 频率 | 400 kHz |

---

## STC8H1K28（背光 + Buzzer + 音频控制）

| 参数 | 值 |
|------|----|
| I2C 地址 | **0x30** |
| I2C 总线 | 与 GT911 共用，SDA=IO15, SCL=IO16 |

### 命令表

| 命令字节 | 功能 |
|---------|------|
| `0x01` | 背光控制前导（先发此，再发亮度） |
| `0x05` | 背光最亮 |
| `0x10` | 背光最暗 |
| `0x15` | **Buzzer ON** |
| `0x16` | **Buzzer OFF** |
| `0x17` | 音频 Unmute |
| `0x18` | 音频 Mute |

### 背光控制代码

```cpp
Wire.begin(15, 16);          // 初始化 I2C

// 设置亮度（先发 0x01，再发亮度值）
Wire.beginTransmission(0x30);
Wire.write(0x01);
Wire.endTransmission();
delay(5);
Wire.beginTransmission(0x30);
Wire.write(0x05);  // 最亮
Wire.endTransmission();

// Buzzer
Wire.beginTransmission(0x30);
Wire.write(0x15);  // ON
Wire.endTransmission();
delay(100);
Wire.beginTransmission(0x30);
Wire.write(0x16);  // OFF
Wire.endTransmission();
```

> ⚠️ **没有背光 GPIO** — V1.1 完全通过 I2C 控制，代码中不需要 `setBrightness()` 或任何 `Light_PWM`

---

## RTC（BM8563）

| 参数 | 值 |
|------|----|
| I2C 地址 | **0x51** |
| SDA | IO15 |
| SCL | IO16 |

> 与 GT911（0x5D）和 STC8H1K28（0x30）共用同一 I2C 总线

---

## 其他接口引脚

| 功能 | GPIO |
|------|------|
| UART0 TX | IO43 |
| UART0 RX | IO44 |
| UART1 TX | IO20 |
| UART1 RX | IO19 |
| SPK LRCLK | IO6 |
| SPK BCLK | IO5 |
| SPK SDIN | IO4 |
| MIC SD | IO20 |
| MIC WS | IO2 |
| MIC CLK | IO19 |
| SD MOSI | IO6 |
| SD MISO | IO4 |
| SD CLK | IO5 |

---

## PSRAM 启用

官方代码中启用了 PSRAM（`use_psram = 1`），platformio.ini 需要：

```ini
board_build.arduino.memory_type = qio_opi
build_flags = -DBOARD_HAS_PSRAM
```

---

## 官方 GitHub

`https://github.com/Elecrow-RD/CrowPanel-Advance-5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480`

关键文件：`factory_sourcecode/V1.1/HMI-bigInch5/LovyanGFX_Driver.h`
