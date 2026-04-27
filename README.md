# tiny-reader

A minimal e-paper EPUB reader for the **LilyGo T5 4.7" V2.3 (Touch / H716)** —
ESP32-S3 + 4.7" 960×540 e-paper + GT911 touch + SD card slot. Drop your `.epub`
files on an SD card or upload them wirelessly from your phone/laptop.

## Features

- Reads `.epub` files from the SD card root — both EPUB 2 (NCX TOC) and
  EPUB 3 (HTML5 nav doc), including Project Gutenberg single-spine books
  where TOC entries are anchors inside one big xhtml file
- XHTML parsing → word-wrapping → justified body text (last line of paragraph stays
  ragged, like a printed book)
- GT911 touch nav: tap left half = previous page, tap right half = next page,
  tap top 80px = back to library
- GPIO 21 / SENSOR_VN button:
  - **short press**: back to library / cycle selection
  - **long press (≥ 2 s)**: open the hub (WiFi share mode)
  - **long-long press (≥ 5 s)**: deep sleep (auto-resumes the same book on wake)
- STR_100 (GPIO 0) button: post-render sample → press = back to library when
  reading. Limited to one sample per page render because GPIO 0 is shared
  with the EPD CFG_STR strobe.
- Per-book reading position persisted to SD as `<bookname>.pos`; resumes on reopen
- Last-open book stored in NVS so deep sleep + wake brings you back to the
  exact page you were on
- The **hub**: an on-demand WiFi access point + mobile-friendly web UI for
  managing the library from any browser (upload, delete, download, jump-to-page,
  per-device settings)
- Configurable text density and AP idle timeout, persisted in `/settings.json`

## Hardware

- LilyGo T5 4.7" e-Paper V2.3 (Touch variant, H716) — ESP32-S3-WROOM-1-N16R8,
  16 MB flash, 8 MB OPI PSRAM, 4.7" ED047TC1 panel, GT911 touch
- microSD (FAT32) with `.epub` files at the root

## Build & flash

```sh
pio run -t upload --upload-port /dev/ttyACM0
```

The first build downloads the ESP-IDF toolchain + libs (~5 min). Subsequent
builds are seconds. Monitor with `pio device monitor --port /dev/ttyACM0`.

## The hub (WiFi share mode)

Long-press the GPIO21 / SENSOR_VN button for ≥ 2 seconds → the device spins up
a WiFi access point and shows the credentials on the e-paper.

- **SSID:** `tiny-reader`
- **Password:** `bv-birdy`
- **URL:** `http://192.168.4.1` (recommended) or `http://tiny-reader.local`

The hub web UI lets you:
- list books with each book's current reading position
- upload `.epub` files from your phone or laptop
- delete books (also removes the position sidecar)
- download books back
- **edit reading position per book** (jump to any chapter/page; saved to the
  `.pos` sidecar, picked up on next open)
- **change settings**: text density (compact / medium / loose) and AP idle timeout
- **maintain a TODO list** (persisted on SD as `/todos.json`, max 50 items)

Auto-exits after the configured idle timeout (default 5 min), or hold the
GPIO 21 button ≥ 1.5 s to exit immediately.

### Configuration (`/settings.json`)

The hub writes a small `settings.json` to the SD card root on every save:

```json
{ "density": 1, "ap_idle_minutes": 5 }
```

- `density`: `0` compact (more words per screen), `1` medium (default),
  `2` loose (extra line spacing). Takes effect on the next book open.
- `ap_idle_minutes`: how long the hub stays up without HTTP activity before
  auto-shutting down. 1–120 minutes.

### A note on phone clients

The device runs a captive-portal-style DNS hijack so phones see "online" and
don't auto-disconnect. If a phone still drops the connection (Linux's
NetworkManager-style "no internet" eviction), the workaround is to navigate
directly to `http://192.168.4.1` and ignore the warning, or temporarily
disable the OS connectivity check while in the hub.

## Serial commands

While connected via `pio device monitor`, you can drive the device:

```
help
next                # advance one page (book mode)
prev                # back one page
back                # back to library (book mode)
open <n>            # n = book index in library
goto <ch>           # ch = chapter index (0-based)
dump                # dump current page or library to serial
share               # enter WiFi share mode
stop_share          # exit WiFi share mode
probe_buttons       # 30 s GPIO read of likely button pins (hardware bring-up)
```

The firmware also emits `[PAGE_BEGIN ch=N/M p=A/B] ... [PAGE_END]` markers
around every rendered page, so a host script can scrape exactly what's on
screen as plain text — handy for development without a camera.

## Repo layout

```
src/main.ino                            reader firmware
lib/epub_parse/                         OPF + spine parser, ZIP extractor
                                        (cherry-picked from atomic14/diy-esp32-epub-reader)
lib/miniz/                              ZIP/deflate codec
lib/lilygo_epd47_s3/                    vendored Xinyuan-LilyGO/LilyGo-EPD47
                                        (esp32s3 branch, src/ only)
boards/T5-ePaper-S3.json                custom PIO board definition
platformio.ini                          build config
```

## Credits

- [Xinyuan-LilyGO/LilyGo-EPD47](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47)
  — display driver, GT911 touch, board JSON (vendored as `lib/lilygo_epd47_s3/`)
- [atomic14/diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader)
  — `Epub`, `ZipFile` parsers (cherry-picked into `lib/epub_parse/`)
- [richgel999/miniz](https://github.com/richgel999/miniz) — ZIP codec
- [leethomason/tinyxml2](https://github.com/leethomason/tinyxml2) — XML parser
