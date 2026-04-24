# Hall Effect Sensor Test — Seeed XIAO ESP32S3

Reads analog output from two A1324LUA-T linear Hall Effect sensors and 
detects player token presence based on magnetic polarity.

## Hardware

| Component | Connection |
|-----------|-----------|
| Seeed XIAO ESP32S3 | — |
| A1324LUA-T Sensor #1 (VCC) | 3.3V |
| A1324LUA-T Sensor #1 (GND) | GND |
| A1324LUA-T Sensor #1 (OUTPUT) | Pin **A0** (GPIO1) |
| A1324LUA-T Sensor #2 (VCC) | 3.3V |
| A1324LUA-T Sensor #2 (GND) | GND |
| A1324LUA-T Sensor #2 (OUTPUT) | Pin **A1** (GPIO2) |

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- USB-C cable
- Two A1324LUA-T Hall Effect sensors
- Two game tokens with magnets of opposite polarity (N pole = Player A, S pole = Player B)

## Setup & Run

1. **Clone / open the project**

   ```bash
   cd "Hall Effect test"
   ```

2. **Connect the board**  
   Plug the XIAO ESP32S3 into your computer via USB-C.

3. **Build and upload**

   ```bash
   pio run --target upload
   ```

4. **Open the Serial Monitor**

   ```bash
   pio device monitor
   ```

   Baud rate is set to **115200**. You should see output like:

   ```
   Sensor1 | Voltage: 1.582V  →  空格
   Sensor2 | Voltage: 1.580V  →  空格
   ---
   Sensor1 | Voltage: 3.300V  →  玩家A 到达！
   Sensor2 | Voltage: 1.581V  →  空格
   ---
   ```

5. **Stop streaming** — press `Ctrl+C`.

## Detection Logic

| Voltage Range | State |
|---------------|-------|
| > 2.5V | Player A (N pole detected) |
| < 0.8V | Player B (S pole detected) |
| 1.5V ± 0.5V | Empty square |

## Validation Results

| Condition | Voltage |
|-----------|---------|
| No magnet (baseline) | ~1.58V |
| N pole (Player A token) | ~3.3V |
| S pole (Player B token) | ~0V |

## Project Structure

```
Hall Effect test/
├── src/
│   └── main.cpp        # Arduino sketch — reads two Hall sensors
├── platformio.ini      # Board and framework configuration
└── README.md
```

## How It Works

`main.cpp` reads `analogRead()` from two A1324LUA-T sensors every 500 ms. 
The A1324LUA-T outputs ~VCC/2 (1.65V) with no magnetic field. 
A North pole increases the output voltage; a South pole decreases it. 
By assigning opposite magnet polarities to each player's token, 
the system can distinguish which player is on a given square.
