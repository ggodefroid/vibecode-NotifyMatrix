# NotifyMatrix

NotifyMatrix is firmware for an Adafruit Matrix Portal S3 driving horizontally chained HUB75 LED panels. The current target is a 128x32 HUD made from two 64x32 panels.

It displays:

- a local clock synchronized over NTP,
- MQTT notifications in a green scrolling banner,
- IDFM / PRIM next-departure information for either one line or a carousel of lines.

The project is configured from one local `.env` file. Secrets and line selections are injected at build time by PlatformIO; no generated secret header is tracked.

## Hardware

- Adafruit Matrix Portal S3
- One or more HUB75 panels, configured by `PANEL_MODULE_WIDTH`, `PANEL_MODULE_HEIGHT`, and `PANEL_CHAIN_LENGTH`
- A 5 V power supply sized for the LED panels
- A USB-C data cable

## Software

- PlatformIO CLI or the PlatformIO VS Code extension
- Python, installed with PlatformIO

## Setup

1. Install dependencies:

   ```bash
   pio pkg install
   ```

2. Create your local environment file:

   ```bash
   cp .env.example .env
   ```

3. Edit `.env`:

   - Fill `WIFI_SSID` and `WIFI_PASSWORD`.
   - Set the MQTT broker fields.
   - Set `IDFM_API_KEY`.
   - Choose either single-line mode or carousel mode.

4. Build:

   ```bash
   pio run -e matrixportal_s3
   ```

5. Upload:

   ```bash
   pio run -e matrixportal_s3 -t upload
   ```

6. Monitor serial logs:

   ```bash
   pio device monitor -b 115200
   ```

## Configuration

All user configuration starts in `.env.example`. Copy it to `.env` and edit the local copy. `.env` is gitignored.

### Single-Line Mode

Use this when the right side of the display should show one stop/line pair:

```dotenv
IDFM_SLOT_COUNT=0
IDFM_MONITORING_REF=STIF:StopPoint:Q:15352:
IDFM_LINE_REF=STIF:Line::C00628:
IDFM_LINE_LABEL=2225
IDFM_LINE_CODE=C00628
```

`IDFM_MONITORING_REF` is the SIRI stop reference. `IDFM_LINE_REF` is preferred because it lets PRIM filter the response before it reaches the ESP32.

### Carousel Mode

Use this when the display should rotate through several stop/line pairs:

```dotenv
IDFM_SLOT_COUNT=8
IDFM_SLOT_0_MONITORING_REF=STIF:StopPoint:Q:15352:
IDFM_SLOT_0_LINE_REF=STIF:Line::C00628:
IDFM_SLOT_0_LINE_CODE=C00628
IDFM_SLOT_0_LABEL=2225
IDFM_SLOT_0_PREFER_THEORETICAL=0
```

If you want the UI to display the scheduled departure time instead of a minute countdown for a slot, set `IDFM_SLOT_N_PREFER_THEORETICAL=1` for that slot.

Continue with `IDFM_SLOT_1_*`, `IDFM_SLOT_2_*`, and so on. Up to 16 slots are supported by the build script. `.env.example` contains the current local carousel as a commented example.

## Runtime Behavior

The IDFM carousel never displays a line before that line has a completed API response.

At boot, the firmware fetches the first visible slot. After that it prefetches the next slot while keeping the current slot on screen. Once the next slot response arrives, the current slot stays visible for `IDFM_HOLD_AFTER_PREFETCH_MS`, then the prefetched result is shown.

MQTT notifications take priority over IDFM HTTP calls. Blocking PRIM requests are postponed while a notification animation is active so the banner can keep drawing smoothly.

The matrix is not redrawn constantly when idle. Normal redraws happen only when the UI changes, throttled by `UI_IDLE_REFRESH_MS`; notification animations use `UI_REFRESH_MS`.

## MQTT Notifications

The firmware subscribes to `MQTT_TOPIC_NOTIFY`. Payloads are copied into the notification banner and scrolled from right to left.

Relevant settings:

```dotenv
MQTT_HOST=192.168.1.10
MQTT_PORT=1883
MQTT_USER=
MQTT_PASSWORD=
MQTT_TOPIC_NOTIFY=notifymatrix/notify
```

From a machine on the same network:

```bash
mosquitto_pub -h 192.168.1.10 -t notifymatrix/notify -m "Hello from NotifyMatrix"
```

## Important Files

| File | Purpose |
| --- | --- |
| `platformio.ini` | PlatformIO environment, board, libraries, upload behavior |
| `.env.example` | The only tracked configuration example |
| `scripts/load_env_config.py` | Reads `.env` and injects build-time macros |
| `include/app_config.h` | Display layout, timings, defaults, and compile-time fallbacks |
| `src/main.cpp` | WiFi, MQTT, clock, IDFM flow, and main loop |
| `src/idfm_client.cpp` | PRIM stop-monitoring HTTP client and SIRI JSON parsing |
| `src/idfm_carousel.cpp` | Carousel slot table generated from `.env` macros |
| `src/hub75_matrixportal.cpp` | Matrix Portal S3 / HUB75 rendering backend |

## Upload Notes

For ROM bootloader upload on the Matrix Portal S3:

1. Plug in USB.
2. Hold BOOT.
3. Press and release RESET while still holding BOOT.
4. Release BOOT.
5. Run `pio run -e matrixportal_s3 -t upload`.

If the serial monitor only shows `waiting for download`, press RESET once without BOOT to boot the firmware from flash.

Double-clicking RESET enters UF2 storage mode; that is not the serial bootloader used by PlatformIO.

## Troubleshooting

- If the LED blinks but the monitor looks stale, close and reopen the serial monitor, then press RESET once.
- If panels are black, check 5 V power, HUB75 cable direction, and panel driver flags.
- If colors or rows are wrong, try the optional build flags shown in `platformio.ini`, such as `PANEL_HUB75_DRIVER_FM6126A`, `PANEL_HUB75_DRIVER_ICN2038S`, or `PANEL_HUB75_DISABLE_ROW_E`.
- If IDFM shows `ERR`, check `IDFM_API_KEY`, `IDFM_MONITORING_REF`, `IDFM_LINE_REF`, and the serial logs.
- If WiFi is unstable, reduce panel brightness or pixel color depth and confirm the power supply is not sagging.

## License

Not defined yet.
