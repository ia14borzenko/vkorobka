## ESP32-S3 + ILI9486 (MAR3501) – I80 LCD Demo

### Overview

This project drives a `MAR3501` LCD module (ILI9486 controller, 480x320, 8‑bit parallel bus)
from an ESP32‑S3 using the ESP‑IDF `esp_lcd` I80 (8080) interface.

- **Only hardware I80 path is used** – no GPIO bit‑banging in the main code path.
- The display driver is encapsulated in `Ili9486Display`:
  - `init()` – reset + ILI9486 init sequence via I80
  - `fillScreen(color)` – fill entire screen with a solid RGB565 color
  - `fillRect(x, y, w, h, color)` – fill a rectangle in pixels
  - `drawTestPattern()` – render color / grayscale gradients for visual testing
- On boot, `app_main()` initializes I80 + ILI9486 and calls `drawTestPattern()` once
  to show a static test image. Wi‑Fi/TCP code is present but disabled.

### Pins (ESP32‑S3 ↔ MAR3501)

8‑bit data bus plus control lines:

| ESP32‑S3 GPIO | MAR pin |
|---------------|---------|
| 4             | LCD_D0  |
| 5             | LCD_D1  |
| 6             | LCD_D2  |
| 7             | LCD_D3  |
| 8             | LCD_D4  |
| 9             | LCD_D5  |
| 10            | LCD_D6  |
| 11            | LCD_D7  |
| 12            | LCD_RS (D/C) |
| 13            | LCD_CS  |
| 14            | LCD_RST |
| 15            | LCD_WR  |
| 16            | LCD_RD  |

### How to build and run

1. Install ESP‑IDF (tested with v5.5.2).
2. From `esp32s3/` run:
   - `idf.py set-target esp32s3`
   - `idf.py -p COMx flash monitor` (replace `COMx` with your port).
3. On boot you should see a static test image:
   - top 3/4 of the screen – smooth RGB gradient (R: left→right, G: top→bottom, B: right→left),
   - bottom area – horizontal grayscale ramp (black→white).

### Notes

- `LCD_DEBUG_NO_NET` is defined in `main.cpp`, so Wi‑Fi/TCP initialization is currently **disabled**.
  You can later remove or comment this macro to enable the existing Wi‑Fi/TCP code.
- The ILI9486 is configured for **RGB565** (`0x3A = 0x55`) and display orientation (`0x36 = 0xE8`)
  to match a landscape layout: `(0,0)` is the top‑left corner, `(479,319)` is bottom‑right.
