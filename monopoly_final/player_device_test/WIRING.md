# Monopoly Player Device — Wiring Guide

**MCU:** Seeed XIAO ESP32S3  
**Framework:** Arduino (PlatformIO)

---

## 1. OLED Display — SSD1351 (128×128, SPI)

> The SSD1351 module's RST pin is tied directly to **3.3 V** on the module itself,
> so no GPIO is consumed for reset. This frees up **D3** for the DOWN button.

| OLED Pin | Wire to | XIAO ESP32S3 Pin | Notes |
|----------|---------|-----------------|-------|
| VCC      | →       | 3.3V            | Power |
| GND      | →       | GND             | Ground |
| MOSI     | →       | D10 (GPIO 9)    | Hardware SPI MOSI |
| SCK      | →       | D8  (GPIO 7)    | Hardware SPI SCK |
| CS       | →       | D1  (GPIO 1)    | Chip Select |
| DC       | →       | D2  (GPIO 2)    | Data/Command |
| RST      | →       | 3.3V            | **Tie to 3.3 V, NOT to a GPIO** |

**MISO (D9 / GPIO 8) is NOT connected to the OLED** — the display is write-only.
D9 is reused as the OK button.

---

## 2. NFC Reader — PN532 (I2C, 4-pin breakout)

> Before wiring, set the DIP switch on the PN532 module to **I2C mode**:  
> **SW1 = OFF, SW2 = ON** (check the silkscreen on your specific module).

| PN532 Pin | Wire to | XIAO ESP32S3 Pin | Notes |
|-----------|---------|-----------------|-------|
| VCC       | →       | 3.3V            | Power |
| GND       | →       | GND             | Ground |
| SDA       | →       | D4  (GPIO 5)    | Hardware I2C SDA |
| SCL       | →       | D5  (GPIO 6)    | Hardware I2C SCL |

No external pull-up resistors are needed — the XIAO's internal pull-ups on the
I2C lines are sufficient for short breadboard traces.

---

## 3. Push Buttons — 5× Tactile Switch (Active LOW)

All buttons use **INPUT_PULLUP** (internal pull-up enabled in firmware).  
Connect one leg of each button to the XIAO pin; connect the other leg to **GND**.

| Button  | XIAO Pin | GPIO  | Action (short press) | Action (long press > 800 ms) |
|---------|----------|-------|----------------------|------------------------------|
| UP      | D0       | GPIO 0 | Scroll up / prev item | — |
| DOWN    | D3       | GPIO 3 | Scroll down / next item | — |
| LEFT    | D6       | GPIO 43 | Back to HOME | Back to HOME |
| RIGHT   | D7       | GPIO 44 | Enter next page | — |
| OK      | D9       | GPIO 8  | Confirm / Select | **Advance to next player's turn** |

> **D3 (DOWN button)** is available because the OLED RST is wired to 3.3 V
> instead of a GPIO. If your SSD1351 module has RST connected to a pin by
> default, cut/lift that trace or use a module that supports hardware reset via
> VCC tie.

---

## 4. Power

| Source   | Supplies           |
|----------|--------------------|
| XIAO 3.3V pin | OLED VCC, PN532 VCC |
| XIAO GND      | OLED GND, PN532 GND, all button GND legs |

The XIAO ESP32S3 can be powered via USB-C during development.
Total current draw (OLED backlight + PN532 + MCU) is under **200 mA**, well
within the USB 500 mA budget.

---

## 5. Pin Assignment Summary

```
XIAO ESP32S3
┌─────────────────────────────────────────┐
│  3.3V ──── OLED VCC                     │
│  3.3V ──── PN532 VCC                    │
│  3.3V ──── OLED RST  (hardware reset)   │
│  GND  ──── OLED GND / PN532 GND         │
│            / all button other legs      │
│                                         │
│  D0  (GPIO 0)  ──── BTN_UP             │
│  D1  (GPIO 1)  ──── OLED CS            │
│  D2  (GPIO 2)  ──── OLED DC            │
│  D3  (GPIO 3)  ──── BTN_DOWN           │
│  D4  (GPIO 5)  ──── PN532 SDA          │
│  D5  (GPIO 6)  ──── PN532 SCL          │
│  D6  (GPIO 43) ──── BTN_LEFT           │
│  D7  (GPIO 44) ──── BTN_RIGHT          │
│  D8  (GPIO 7)  ──── OLED SCK           │
│  D9  (GPIO 8)  ──── BTN_OK             │
│  D10 (GPIO 9)  ──── OLED MOSI          │
└─────────────────────────────────────────┘
```

---

## 6. First-time NFC Card Mapping

The firmware prints the UID of any unknown card to the serial monitor at
**115200 baud**. To map a new physical card to a board square:

1. Open the PlatformIO serial monitor: `pio device monitor -b 115200`
2. Tap the card on the PN532 reader.
3. Read the `[NFC] UID: XX XX XX XX` line.
4. Add the UID to the `nfcCards[]` array in `src/main.cpp`:

```cpp
{ { 0xXX, 0xXX, 0xXX, 0xXX }, <squareIdx> },
```

Square indices are defined in the `squares[]` array (index 0 = Go, etc.).

---

## 7. Library Dependencies (auto-installed by PlatformIO)

| Library | Version |
|---------|---------|
| Adafruit SSD1351 library | ≥ 1.3.2 |
| Adafruit GFX Library | ≥ 1.11.9 (auto dependency) |
| Adafruit PN532 | ≥ 1.3.2 |

Run `pio pkg install` or simply `pio run` — PlatformIO resolves and installs
all dependencies automatically from `platformio.ini`.
