# nes-rom-reader

[日本語](README.md) | **English** | [简体中文](README.zh.md)

A Famicom (FC 60-pin) cartridge ROM dumper. M5Stamp S3 + custom PCB (designed in EasyEDA Pro) + Python CLI / Web UI.

### ▶ Try it now in your browser (no install): **https://goroman.github.io/nes-rom-reader/web/**
Open it in Chrome / Edge (desktop or Android) and just connect your M5Stamp S3 to start dumping.

![Dumping on real hardware](docs/images/hero-dumping.jpeg)

> Dumping a real cartridge (Ganso Saiyuki: Super Monkey Daibouken / VAP 1986) on actual hardware,
> with the CHR-ROM tiles shown in the Web UI's "Super Monkey Viewer".

![Routed PCB](docs/images/pcb-routed.png)

## Layout

| Directory | Contents |
|---|---|
| `firmware/` | M5Stamp S3 (ESP32-S3) firmware (PlatformIO / Arduino) |
| `host/` | Python CLI (dumper `famidump.py` / factory test `factory_test.py`) |
| `web/` | Browser Web UI (WebSerial — dump + factory test, no install) |
| `hardware/` | EasyEDA Pro project, Gerber, BOM, pick-and-place |
| `docs/` | Design docs (pin map, bus design, 60-pin pinout) |

## Architecture

- **MCU**: M5Stamp S3 mounted on a **1.27 mm-pitch socket** (removable via female headers)
- **Cartridge I/F**: FC 60-pin (2.54 mm pitch) edge connector, pads placed directly on the board
- **Bus drivers**:
  - Address lines … 74HCT595 ×4 shift-register chain (5 V)
  - Data read … 74LVC245 ×2 (PRG/CHR, 5 V→3.3 V level shift)
  - Control lines … 74HCT541 buffer (3.3 V→5 V)
- **Mappers**: Mapper 0 (NROM) first; CNROM / UNROM / MMC1 planned
- **Host**: Python CLI (`host/famidump.py`) writes a `.nes` file with an iNES header, CRC32-verified

## Supported cartridges & limitations

- ✅ **Mapper 0 (NROM)** … fully supported (PRG 32KB + CHR 8KB). Verified on real hardware.
- ⚠️ **Bank-switched mappers (CNROM / UNROM / MMC1, etc.) are NOT supported yet.**

**Why (important)**: bank switching requires **writing** to the mapper register, but this board's data
buffers (74LVC245) are **fixed-direction, read-only (cartridge → ESP32)**. Without write capability we
cannot switch CHR/PRG banks.

Example: **Transformers: Convoy no Nazo** (CNROM / mapper 3) — its 32KB PRG can be dumped, but only the
power-on CHR bank (8KB) is readable; the other CHR banks require a register write to select.

→ Supporting CNROM/UNROM/MMC1 needs a **v0.2 board with bidirectional (write-capable) data buffers** plus
firmware that performs the bank switching.

## Board

- 2-layer, ground pour on both sides, approx. 84 × 77 mm
- USB-C notch at the bottom-center, M3 mounting holes in the four corners (for a 3D-printed enclosure)
- Fab data: upload [`hardware/gerber/nes-rom-reader-gerber.zip`](hardware/gerber/) straight to JLCPCB etc.

## Usage

```sh
# Flash the firmware
cd firmware && pio run -t upload

# Dump (NROM: PRG 32KB + CHR 8KB)
python host/famidump.py --port /dev/tty.usbmodem* --prg 32 --chr 8 -o game.nes
```

## Web UI (dump from your browser)

![Web UI](docs/images/webui.png)

Just open `web/index.html` in **desktop Chrome / Edge**. It connects to the M5Stamp S3 over WebSerial and lets you
dump, download `.nes`, and run the factory test from a GUI (no Python). Famicom color scheme & dot font,
with **Japanese / English toggle**. Also works on **Android Chrome** (responsive).

### 🐵 Super Monkey Viewer (CHR-ROM viewer)

![CHR Viewer](docs/images/chr-viewer.png)

Decodes the dumped CHR-ROM (2bpp planar format) into 8×8 tiles. You can freely change the 4 colors from the
NES 64-color master palette, and switch presets (Grayscale / Game Boy / Mario, etc.). It also loads `.nes` / `.chr`
files, so you can browse offline without hardware.

```sh
# Serve locally (file:// works too, but WebSerial prefers https or localhost)
cd web && python3 -m http.server 8000   # → http://localhost:8000
```

## Factory Test (100% inspection)

Verifies each assembled board by comparing the dumped CRC of a **reference cartridge (known CRC)**.

```sh
# 1) Learn the reference CRC on a known-good board (with the reference cartridge inserted)
python host/factory_test.py --port /dev/cu.usbmodem* --learn --name "MyRefCart"

# 2) Test each board (same reference cartridge). Exit code 0 = PASS / 1 = FAIL
python host/factory_test.py --port /dev/cu.usbmodem* --test --name "MyRefCart"

# Self-test only, no cartridge needed (GPIO / shift-register health)
python host/factory_test.py --port /dev/cu.usbmodem* --selftest
```

The firmware `T` command (self-test) reads back output pins, exercises the shift registers, reads the
floating data bus, and finally returns `SELFTEST PASS/FAIL`. The same check runs from the Web UI's "Factory Test" tab.

## Documentation

- Design notes, pin map, 60-pin pinout: [docs/hardware-design.md](docs/hardware-design.md)
- Board data details: [hardware/README.md](hardware/README.md)

## Credits

This project's firmware development and PCB design in **EasyEDA Pro** (schematic, PCB, routing, and fab-data
generation) were done by **[Claude Code](https://claude.com/claude-code) (Fable 5)**.

Claude Code drives EasyEDA Pro through the WebSocket bridge extension
**[eext-run-api-gateway](https://github.com/easyeda/eext-run-api-gateway)**, which lets the AI operate
EasyEDA Pro directly to design the board.

## Links

- Repository: https://github.com/GOROman/nes-rom-reader
- Web UI (live): https://goroman.github.io/nes-rom-reader/web/
- Author on X (Twitter): [@GOROman](https://x.com/GOROman)

---

Powered by Claude Code (Fable 5) × EasyEDA Pro
