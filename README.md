# esp32mass

Firmware for the LilyGO T-Dongle-S3 that turns the dongle into a USB mass
storage device whose contents are a raw disk image uploaded over Wi-Fi.

* The microSD card **is** the drive. Images are written from sector 0, exactly
  like Rufus or `dd` would write a USB stick, and the full card is exposed to
  the USB host read/write.
* First boot (or a lost network) brings up a Wi-Fi access point with a captive
  portal where you pick your network. After that the UI is at
  `http://esp32mass.local/` or the IP shown on the little screen.
* Uploads are chunked and resumable, hashed on both ends, and optionally read
  back and verified. The USB drive is detached while writing and re-attached
  when the image is complete.
* The web UI also offers: attach/detach, read-only toggle, download the image
  back, wipe the partition table, live log, and firmware update (OTA).

## Hardware facts that shaped the design

| Item | Value |
|---|---|
| SoC | ESP32-S3, 16 MB quad flash, **no PSRAM** (about 320 KB usable RAM) |
| USB | Native OTG, Full Speed only (12 Mbit/s, roughly 1 MB/s for the host) |
| SD | 4-bit SDMMC on GPIO 12/16/14/17/21/18 |
| LCD | ST7735 80x160 on SPI2 (GPIO 3/5/4/2/1, backlight 38 active low) |
| LED | APA102 on GPIO 40 (data) / 39 (clock) |
| Button | GPIO 0 (BOOT) |

## Building

Native ESP-IDF v5.5.x. The project pulls `espressif/tinyusb` and
`espressif/mdns` from the component registry on first configure.

Ubuntu/WSL prerequisites (ESP-IDF's `export.sh` refuses to run without
openocd, and openocd needs libusb):

```sh
sudo apt install git wget flex bison gperf python3-venv cmake ninja-build ccache \
     libffi-dev libssl-dev dfu-util libusb-1.0-0
cd ~/esp/esp-idf && ./install.sh esp32s3
```

Then:

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

If the project lives on a Windows drive under WSL, build into a Linux path for
speed: `idf.py -B ~/esp/build-esp32mass build`.

Ready-made binaries land in `firmware/`:

* `esp32mass-full-<ver>.bin` — whole flash image, write at offset 0x0
* `esp32mass-ota-<ver>.bin` — app only, for the web UI's firmware update

## Flashing

The dongle's only connector is the USB plug, and once this firmware runs,
TinyUSB owns it, so the ROM serial bootloader is not visible. To flash:

1. Hold BOOT while plugging the dongle in. It enumerates as the Espressif
   USB-Serial/JTAG device (`/dev/ttyACM0` on Linux, a COM port on Windows).
2. `idf.py -p /dev/ttyACM0 flash`
3. Unplug and replug without BOOT.

After the first flash, use the **Firmware update** box in the web UI with
`build/esp32mass.bin` instead.

From WSL2 the port is only visible after `usbipd attach`. The simpler route is
to flash from Windows with esptool (`pip install esptool`):

```powershell
esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 firmware\esp32mass-full-0.1.0.bin
```

## Using it

* **LED**: blue blink = access point up, waiting for setup; amber blink =
  connecting to Wi-Fi; green = attached to USB (brighter on host I/O); purple
  breathing = upload in progress; red blink = no SD card.
* **Button**: short press toggles USB attach/detach. Hold 5 s to forget the
  Wi-Fi credentials and reboot into setup mode.
* **Access point**: `esp32mass-XXXX`, password `esp32mass` (changeable in the
  UI, or set empty for an open network). Portal at `http://192.168.4.1/`.

## Serial console

UART0 (GPIO 43 TX / 44 RX) is on the QWIIC connector. You do not need it:
the log is available in the UI and at `GET /api/log`.

## HTTP API

| Method | Path | Body / notes |
|---|---|---|
| GET | `/api/status` | everything the UI shows |
| GET | `/api/scan` | Wi-Fi scan (blocks a few seconds) |
| POST | `/api/wifi` | `{"ssid","pass"}` |
| POST | `/api/wifi/forget` | |
| POST | `/api/settings` | any of `read_only`, `auto_attach`, `verify`, `hostname`, `ap_pass` |
| POST | `/api/usb` | `{"action":"attach"|"detach"|"replug"}` |
| POST | `/api/upload/begin` | `{"name","size"}` detaches USB |
| PUT | `/api/upload/chunk?offset=N` | raw bytes, sequential; 409 with `received` on mismatch |
| POST | `/api/upload/finish` | `{"sha256"?, "verify"?}` re-attaches USB |
| POST | `/api/upload/abort` | |
| GET | `/api/download` | the recorded image (`?full=1` for the whole card); USB must be detached |
| POST | `/api/wipe` | zero the first 2 MB |
| GET | `/api/log` | recent log text |
| POST | `/api/ota` | raw app `.bin` |
| POST | `/api/reboot` | |

Example with curl:

```sh
curl -X POST http://esp32mass.local/api/upload/begin -d '{"name":"disk.img","size":104857600}'
curl -T disk.img "http://esp32mass.local/api/upload/chunk?offset=0"
curl -X POST http://esp32mass.local/api/upload/finish -d '{}'
```

## Layout on the card

Nothing but the image. Metadata (name, size, SHA-256, time) lives in NVS on
the ESP32's flash, not on the card. A card written elsewhere works too; it
just shows "none recorded" for the image.
