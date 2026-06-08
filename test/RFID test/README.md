# Dual RFID RC522 Test — Seeed XIAO ESP32S3

Streams card UIDs from two MFRC522 RFID readers over USB Serial in real time.

## Hardware

Both readers share the board's default SPI bus (SCK / MISO / MOSI). Each reader has its own Slave-Select (SS) and Reset (RST) pin.

| Signal | RFID Reader 1 | RFID Reader 2 | XIAO ESP32S3 Pin |
|--------|--------------|--------------|-----------------|
| SDA (SS) | SDA | — | **D0** |
| RST | RST | — | **D5** |
| SDA (SS) | — | SDA | **D1** |
| RST | — | RST | **D6** |
| SCK | SCK | SCK | SCK (shared) |
| MISO | MISO | MISO | MISO (shared) |
| MOSI | MOSI | MOSI | MOSI (shared) |
| 3.3 V | VCC | VCC | 3V3 |
| GND | GND | GND | GND |

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- USB-C cable
- Two MFRC522 RFID modules and compatible RFID cards / tags

## Setup & Run

1. **Clone / open the project**

   ```bash
   cd "RFID test"
   ```

2. **Connect the board**  
   Plug the XIAO ESP32S3 into your computer via USB-C. Wire both RFID modules according to the table above.

3. **Build and upload**

   ```bash
   pio run --target upload
   ```

   PlatformIO will automatically download the `espressif32` platform and the `MFRC522` library (`miguelbalboa/MFRC522 @ ^1.4.10`) on first run.

4. **Open the Serial Monitor**

   ```bash
   pio device monitor
   ```

   Baud rate is **115200**. After reset you should see:

   ```
   Dual RFID RC522 Test
   RFID1 initialized
   RFID2 initialized
   Place a card near reader 1 or reader 2...
   ```

5. **Scan a card**  
   Hold an RFID card close to either reader. The UID is printed immediately:

   ```
   RFID1 detected card. UID: A3 4F 12 80
   RFID2 detected card. UID: 7B 2E 99 01
   ```

6. **Stop streaming** — press `Ctrl+C` in the Serial Monitor.

## Project Structure

```
RFID test/
├── src/
│   └── main.cpp        # Arduino sketch — polls both readers and streams UIDs
├── platformio.ini      # Board, framework, and library configuration
└── README.md
```

## How It Works

`main.cpp` initializes two `MFRC522` instances on separate SS/RST pins but the same SPI bus. Inside `loop()`, `checkReader()` is called for each reader in turn. When a new card is detected, its UID bytes are printed in hexadecimal, then the reader is halted so it is ready for the next card. The poll interval between readers is 100 ms.
