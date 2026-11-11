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
`esp32mass.bin` from the build directory (`build/`, or wherever `-B` points)
or `firmware/esp32mass-ota-<ver>.bin` instead.

Web-UI updates can roll back. A new image boots on probation and is confirmed
the first time the web UI (`/api/status`) or `/api/ota` is reached; if the
device is reset before that, the bootloader returns to the previous firmware.
The UI polls status, so keeping the page open through the reboot confirms the
update. Rollback needs a bootloader built with it enabled, so it applies only
after one serial flash of a full image built from this configuration.

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
| POST | `/api/upload/finish` | `{"sha256"?, "verify"?}` re-attaches USB; with verify on, returns `202` and reads the card back in the background: poll `/api/status` until `upload.verifying` is false, then `upload.error` / `upload.verified` give the result |
| POST | `/api/upload/abort` | |
| GET | `/api/download` | the recorded image (`?full=1` for the whole card); USB must be detached |
| POST | `/api/wipe` | zero the first 2 MB and last 1 MB (partition tables incl. backup GPT) |
| GET | `/api/log` | recent log text |
| POST | `/api/ota` | raw app `.bin` |
| POST | `/api/reboot` | |

Every POST and PUT must send an `X-Requested-With` header (any value). Browsers
cannot add that header to a cross-site request without a CORS preflight, which
the device never approves, so other websites cannot drive the API through your
browser. Requests without it get `403`.

Example with curl. Send the image in pieces: the ESP-IDF HTTP server stores
a request's body length as a 32-bit value, so a single PUT of 4 GiB or more is
silently truncated. Each piece below is read into memory by curl, so keep
`CHUNK_MB` modest.

```sh
H='X-Requested-With: curl'
URL=http://esp32mass.local
IMG=disk.img
CHUNK=$((64 * 1048576))
START=0   # to resume an interrupted upload, set to upload.received from /api/status
SIZE=$(stat -c %s "$IMG")

[ "$START" -eq 0 ] && { curl -fsS -H "$H" -X POST "$URL/api/upload/begin" \
     -d "{\"name\":\"$(basename "$IMG")\",\"size\":$SIZE}" || exit 1; }
for ((off = START; off < SIZE; off += CHUNK)); do
  dd if="$IMG" iflag=skip_bytes,count_bytes skip=$off count=$CHUNK bs=1M status=none |
    curl -fsS -H "$H" -X PUT --data-binary @- "$URL/api/upload/chunk?offset=$off" || exit 1
done
curl -fsS -H "$H" -X POST "$URL/api/upload/finish" \
     -d "{\"sha256\":\"$(sha256sum "$IMG" | cut -d' ' -f1)\"}" || exit 1
# With read-back verify on, finish returns 202 and the card is checked in the background.
until curl -fsS "$URL/api/status" | grep -q '"verifying":false'; do sleep 2; done
curl -fsS "$URL/api/status" | grep -o '"error":"[^"]*","verifying":false,[^}]*'
```

`--data-binary @-` (not `-T -`) matters: piping into `-T` uses chunked transfer
encoding, which the device does not accept. If a piece fails, the device keeps
what it has written so far (to a buffer boundary, not necessarily the start of
the failed piece); `GET /api/status` reports it as `upload.received`, which is
the `START` to resume from. The device drops an upload that sees no data for
2 minutes.

## Layout on the card

Nothing but the image. Metadata (name, size, SHA-256, time) lives in NVS on
the ESP32's flash, not on the card. A card written elsewhere works too; it
just shows "none recorded" for the image.
