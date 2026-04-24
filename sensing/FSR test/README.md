# FSR Sensor Test — Seeed XIAO ESP32S3

Streams raw analog readings from a Force-Sensitive Resistor (FSR) over USB Serial at 10 Hz.

## Hardware

| Component | Connection |
|-----------|-----------|
| Seeed XIAO ESP32S3 | — |
| FSR (one leg) | Pin **D0** (GPIO 2) |
| FSR (other leg) | **GND** |
| 10 kΩ pull-down resistor | Between **D0** and **GND** |

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- USB-C cable

## Setup & Run

1. **Clone / open the project**

   ```bash
   cd "FSR test"
   ```

2. **Connect the board**  
   Plug the XIAO ESP32S3 into your computer via USB-C. Identify the port (e.g., `COM3` on Windows, `/dev/ttyUSB0` on Linux/macOS).

3. **Build and upload**

   ```bash
   pio run --target upload
   ```

   PlatformIO will automatically download the `espressif32` platform and the `MFRC522` library listed in `platformio.ini` on first run.

4. **Open the Serial Monitor**

   ```bash
   pio device monitor
   ```

   Baud rate is set to **115200**. You should see a stream of integer values (0–4095) printed once every 100 ms.

   ```
   312
   315
   890
   2047
   ...
   ```

   Higher values indicate more pressure on the FSR.

5. **Stop streaming** — press `Ctrl+C` in the Serial Monitor.

## Project Structure

```
FSR test/
├── src/
│   └── main.cpp        # Arduino sketch — reads FSR and streams data
├── platformio.ini      # Board and framework configuration
└── README.md
```

## How It Works

`main.cpp` calls `analogRead(D0)` every 100 ms inside `loop()` and prints the raw 12-bit ADC value (0–4095) to `Serial`. No filtering or calibration is applied; the raw value scales linearly with the voltage divider formed by the FSR and the pull-down resistor.
