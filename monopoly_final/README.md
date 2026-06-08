# Monopoly Final — Smart Board Game System

A 2-player digital Monopoly system built with ESP32-S3 microcontrollers, featuring a central bank display and handheld player devices communicating over BLE.

## System Architecture

```
┌─────────────────────────┐       BLE        ┌─────────────────────────┐
│   Central Bank Display  │◄────────────────►│   Player Card (P1)      │
│   CrowPanel 5" 800×480  │                  │   XIAO ESP32S3          │
│   ESP32-S3 (game master)│       BLE        │   SSD1351 128×128 OLED  │
│                         │◄────────────────►│   PN532 NFC + 5 buttons │
└─────────────────────────┘                  ├─────────────────────────┤
                                             │   Player Card (P2)      │
                                             │   (same hardware)       │
                                             └─────────────────────────┘
```

## Hardware

| Component | Central Bank | Player Card |
|-----------|-------------|-------------|
| MCU | ESP32-S3 (CrowPanel 5") | Seeed XIAO ESP32S3 |
| Display | 5" IPS 800×480 (RGB parallel, LovyanGFX) | SSD1351 128×128 color OLED (SPI) |
| Input | Touchscreen (GT911) | 5 buttons (OK, LEFT, RIGHT, UP, DOWN) |
| NFC | — | PN532 (I2C) |
| Communication | BLE server (NimBLE) | BLE client (NimBLE) |

## Folder Structure

```
monopoly_final/
├── central_bank/          # Central bank display (PlatformIO project)
│   ├── platformio.ini
│   └── src/main.cpp       # Game master: state machine, dice, BLE server, UI
├── player_card/           # Player handheld device (PlatformIO project)
│   ├── platformio.ini     # Separate build envs for P1 and P2
│   └── src/main.cpp       # BLE client, OLED UI, NFC reader, button input
└── README.md
```

## Features

- **Full Monopoly rules**: 40-square UK board, property buying, rent, tax, Chance/Community Chest, jail, auctions
- **BLE communication**: Central bank acts as BLE server; player devices connect as clients
- **Dice rolling**: Dice animation on the central display, results broadcast to players
- **Property auctions**: When a player declines to buy, an auction starts with countdown timer
- **NFC payment**: Player-to-player rent payment confirmed via NFC tag tap
- **Ownership tracking**: Central bank tracks all property ownership; players can view their properties
- **Bankruptcy detection**: Automatic game-over when a player goes bankrupt

## BLE Protocol

- **Service UUID**: `0000AA01-0000-1000-8000-00805F9B34FB`
- **Notify characteristic** (central → players): `0000BB01-...`
- **Write characteristic** (players → central): `0000BB02-...`

Key messages:

| Direction | Message | Description |
|-----------|---------|-------------|
| Central → Player | `TURN:{n}` | Player n's turn |
| Central → Player | `DICE:{sum}` | Dice result |
| Central → Player | `ACT:BUY:{name}:{price}` | Purchase option |
| Central → Player | `ACT:RENT:{amt}:{owner}` | Pay rent |
| Central → Player | `STATE:{p1pos}:{p1bal}:{p2pos}:{p2bal}` | Full state sync |
| Player → Central | `P{n}:ROLL` | Request dice roll |
| Player → Central | `P{n}:BUY` / `P{n}:SKIP` | Buy or skip property |

## Building & Flashing

Requires [PlatformIO](https://platformio.org/).

### Central Bank Display

```bash
cd central_bank
pio run -t upload
pio device monitor
```

### Player Cards

Flash Player 1:
```bash
cd player_card
pio run -e xiao_esp32s3_p1 -t upload
```

Flash Player 2:
```bash
cd player_card
pio run -e xiao_esp32s3_p2 -t upload
```

The player number is set via `-DPLAYER_NUM=1` or `-DPLAYER_NUM=2` in the build flags.

## Wiring (Player Card)

| OLED Pin | XIAO Pin | GPIO |
|----------|----------|------|
| DIN/MOSI | D10 | GPIO9 |
| CLK/SCK | D8 | GPIO7 |
| CS | D1 | GPIO1 |
| DC | D2 | GPIO2 |
| RST | D3 | GPIO3 |

PN532 NFC is connected via I2C (default SDA/SCL pins on XIAO ESP32S3).

## Team

- **Sienna** — board sensing, central display, game logic
- **Polly** — player device, BLE integration
