# Bring-up firmware (r5 boards)

MicroPython v1.29.0, generic ESP32-S3 build (works with the 4 MB-flash N4 modules fitted to the first boards),
plus `main.py`, which blinks the STATUS LED (D101, GPIO2) at 2 Hz and leaves every isolation switch open.

## Flash a board
1. `pip install esptool` (any Python environment).
2. Download `ESP32_GENERIC_S3-20260824-v1.29.0.bin` from https://micropython.org/download/ESP32_GENERIC_S3/
   (sha256 starts `1dd9dcd27ba2d6a1`).
3. With the board on USB (it shows up as `303a:1001 USB JTAG/serial debug unit`):
   ```
   esptool --chip esp32s3 -p /dev/ttyACM0 erase-flash
   esptool --chip esp32s3 -p /dev/ttyACM0 --baud 921600 write-flash 0 ESP32_GENERIC_S3-20260824-v1.29.0.bin
   ```
   No BOOT button is needed: esptool resets the chip into download mode over USB. If "port is busy" appears,
   retry (Linux ModemManager briefly probes new serial ports).
4. The board re-appears as `303a:4001` (MicroPython). Copy `main.py` to it, e.g. `mpremote cp main.py :main.py`,
   then `mpremote reset`.

## Expected behaviour of an empty board
`invalid header: 0xffffffff` printed repeatedly and a USB reconnect every ~3 s (`rst:0x7 TG0WDT_SYS_RST`) is normal
for blank flash. It stops once firmware is written.

## Bring-up log
| Board MAC | Module | Result |
|---|---|---|
| f0:f5:bd:57:ef:48 | ESP32-S3 v0.2, 4 MB flash (N4) | USB failed with `error -71` until module pad 13 (D-) was re-soldered; then OK |
| f0:f5:bd:57:ef:34 | ESP32-S3 v0.2, 4 MB flash (N4) | hot-air soldered, OK first time; MicroPython + blink flashed 2026-11-06 |
