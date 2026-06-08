# Monopoly Final — Project Notes

## Hardware

- **Central Display**: Seeed XIAO ESP32S3 + SSD1351 128x128 color OLED + BLE server
- **Player Device**: Seeed XIAO ESP32S3 + SSD1351 128x128 color OLED + PN532 NFC (I2C) + 5 buttons + BLE client

---

## Known Fixes

### OLED Black Screen (Player Device)

**Symptoms**: OLED stays black after flashing.

**Root causes & fixes (in order)**:

1. **SPI pins not explicitly initialized**
   - Fix: call `SPI.begin(D8, D9, D10, OLED_CS)` before `display.begin()`
   - XIAO ESP32S3 does not always auto-configure SPI2 pins correctly

2. **RST pin floating (connected to 3V3)**
   - Fix: connect OLED RST to a GPIO (e.g. D3), define `#define OLED_RST D3`, and pass it to the constructor: `Adafruit_SSD1351 display(128, 128, &SPI, OLED_CS, OLED_DC, OLED_RST)`
   - Without a proper LOW pulse on RST at boot, the SSD1351 fails to initialize

3. **NimBLE `pScan->start(0)` crashes ESP32S3**
   - `start(0)` (infinite scan) causes a stack overflow crash in the NimBLE host task
   - Fix: use timed scan `pScan->start(10, nullptr, false)` and restart in loop when scan stops
   - Also add to `platformio.ini`: `-DCONFIG_BT_NIMBLE_TASK_STACK_SIZE=5120`

### Working OLED Pin Mapping (Player Device)

| OLED Pin | XIAO Pin | GPIO |
|----------|----------|------|
| DIN/MOSI | D10      | GPIO9 |
| CLK/SCK  | D8       | GPIO7 |
| CS       | D1       | GPIO1 |
| DC       | D2       | GPIO2 |
| RST      | D3       | GPIO3 |
| VCC      | 3V3      | —     |
| GND      | GND      | —     |

### Working platformio.ini (Player Device)

```ini
board_build.partitions = min_spiffs.csv
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCONFIG_BT_NIMBLE_TASK_STACK_SIZE=5120
```

---

## BLE

- Central (server) advertises as `"MonopolyCentral"`
- Player (client) scans and connects, registers with `"P1:HELLO"`
- Service UUID: `0000AA01-0000-1000-8000-00805F9B34FB`
- Notify characteristic: `0000BB01-0000-1000-8000-00805F9B34FB`
- Write characteristic: `0000BB02-0000-1000-8000-00805F9B34FB`
