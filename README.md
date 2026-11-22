# SL6801 iPod — Reverse Engineering & Custom Firmware

Custom firmware, reverse-engineering notes, flash tools, and iPod Classic UI work for a budget **Smartlink SL6801** touchscreen MP3 player (`yp3_2.0.43`, 240×320 ST7789S + CST726).

---

## 1. Device Identification & Discovery

### How we identified the device
1. **USB Enumeration (Normal / Card Reader Mode):**
   - Connected via USB-C on macOS (`system_profiler SPUSBDataType` / `diskutil list` / `ioreg -p IOUSB`).
   - Enumerated as **`301a:2801`** (`SmartlinkTechnology`, `USB2.0 Device`, serial `20201111000001`), exposing only a single USB Mass Storage interface (`08/06/50` SCSI Bulk-Only) mounted as `/Volumes/TOONZ` (32 GB FAT32 microSD).
   - No USB serial/UART port (`/dev/cu.usb*`) is exposed.
2. **Entering Bootloader / DFU Mode (`301a:2800`):**
   - Power the player completely **off**.
   - **Hold down the `VOLUME UP` key ONLY** *before* plugging in USB-C, and **keep holding it down while plugging in the USB-C cable**.
   - Enumerates as **`301a:2800`** (`SmartlinkTechnology`, serial `20201111000001`).
   - IOKit SCSI inquiry reports **`SMTLINK DEVICE 2.00`** — confirming the **Smartlink / Shenzhen Shenju SL6801** boot ROM.
3. **Talking to the Boot ROM on macOS:**
   - Compiled [`ilyakurdyukov/smartlink_flash`](https://github.com/ilyakurdyukov/smartlink_flash) (`smartlink_flash/smtlink_dump.c`) against Homebrew `libusb-1.0`.
   - macOS's `IOUSBMassStorageDriver` claims `301a:2800` by default, returning `LIBUSB_ERROR_ACCESS` without privileges. Running with `sudo` cleanly detaches the kernel driver and gives full boot-ROM access:
     ```bash
     sudo ./smartlink_flash/smtlink_dump serial inquiry init flash_id
     ```
   - Boot ROM reported **`flash_id: 0x14851485`** — **Puya (`0x85`) 16 Mbit / 2 MB (`0x14`) SPI NOR flash**.
4. **Dumping the Stock Firmware:**
   ```bash
   sudo ./smartlink_flash/smtlink_dump init read_flash 0 2M firmware/sl6801_backup.bin
   ```
   - Verified 2,097,152 bytes (`0x200000`).
   - **SHA256:** `e2a1a6f37f45f88fcabc3bffb2946f7ec11a96daa61e2314df9fadbb4104e8c5`

---

## 2. Hardware & Firmware Capabilities

Extracted via strings analysis, disassembly (`objdump --triple=thumbv7m-none-eabi`), and `reference/SL6801_player/tools/slfw.py`:

- **SoC / CPU:** Smartlink **SL6801** — dual-core ARM Cortex-M4F (CPU0 runs FreeRTOS/audio/FatFS/Bluetooth; CPU1 runs the LVGL 8 UI loop)
- **Stock Firmware:** `yp3_2.0.43` (`HLKJ` boot header)
- **Flash:** 2 MB SPI NOR (`Puya 0x14851485`), XIP mapped at **`0x00C00000 + flash_offset`**
- **Display:** **240×320** RGB565 TFT LCD (**ST7789S**, `st7789s_lcd_init`) with backlight timer
- **Touchscreen:** **CST726** (FocalTech/Hynitron protocol on I2C bus 0, address **`0x38`**)
- **Physical Keys:** Power/OnOff, Vol+, Vol-, `btn0`–`btn3`
- **Wi-Fi:** **None** (no Wi-Fi radio)
- **Bluetooth:** **Bluetooth Classic** (A2DP source, AVRCP, HFP call handling, SDP/L2CAP; no BLE stack in stock firmware)
- **Audio Output:** Built-in speaker + 3.5 mm headphone jack with plug detection (decodes MP3, WAV, FLAC, APE, OGG, AAC, WMA)
- **Audio Input:** Dual analog mic (`AMIC1`/`AMIC2`) with PCM/ADPCM `.wav` recording
- **Video / Images:** MJPEG `.avi` video & JPEG decoding
- **FM Radio:** Built-in FM tuner (`fm_handle`, headphones as antenna)
- **Storage:** microSD card slot (FAT32 via FatFS)
- **Clock / Power:** RTC with alarms, battery ADC monitoring, USB charging
- **USB:** USB 2.0 Full-Speed (12 Mb/s) Mass Storage (`301a:2801`) & ROM Bootloader (`301a:2800`, unbrickable)

---

## 3. Flash & Memory Layout

Parsed with `python3 reference/SL6801_player/tools/slfw.py firmware/sl6801_backup.bin info`:

| Region | Flash Offset | Size | XIP / Load Address | CRC16 (CCITT-FALSE) | Notes |
| --- | --- | --- | --- | --- | --- |
| **Bootloader (`HLKJ`)** | `0x000000` (code `0x60`) | `0x00C8EC` | Load `0x0081FBC0`, Entry `0x00820001` | `0x759D` | Stage-2 bootloader |
| **Partition Table** | `0x00D000` | `0x001000` | — | — | Entries for `FIRM`, `TONE`, `PSMP` |
| **`FIRM` Partition** | `0x00E000` | `0x1EA8F0` | RAM `0x00804C00` (4 KB) + XIP `0x00C00000 + off` | `0xC56E` (at `0x00D01C`) | Main FreeRTOS + LVGL 8 firmware |
| **Free Flash Cave** | `0x018A8E` | **`0x0209E2` (130 KB)** | **`0x00C18A8E`** | Inside `FIRM` | Zero-filled free space for custom C/LVGL code & assets |
| **`TONE` Partition** | `0x1F9000` | `0x002038` | — | `0x9177` (at `0x00D02C`) | `alarm1.mp3` at `0x1F9400` |
| **`PSMP` Partition** | `0x1FC000` | `0x004000` | — | `0x0000` | Persistent NV settings |

- **CRC16 Algorithm:** CRC-16/CCITT-FALSE (`poly=0x1021`, `init=0xFFFF`, no reflection, `xorout=0x0000`).

---

## 4. Key Reverse-Engineered Symbols (in `yp3_2.0.43` 240×320)

| Symbol / Function | File Offset | CPU Address | Notes |
| --- | --- | --- | --- |
| `rom_printf` | ROM | `0x000002B0` | Boot ROM debug printf |
| `delay_ms` | RAM | `0x0080D580` | Millisecond sleep |
| `i2c_write` | RAM | `0x008169DC` | `i2c_write(bus, addr, buf, len, stop)` |
| `i2c_read` | RAM | `0x00816A4C` | `i2c_read(bus, addr, buf, len)` |
| `lv_indev_drv_init` | `0x1ADC7C` | `0x00DADC7C` | Sets `scroll_limit` at `0x1ADC94` (`0x0A` = 10 px) |
| `lv_indev_drv_register` | `0x1ADC9C` | `0x00DADC9C` | Registers LVGL indev with 20 ms (`0x14`) timer |
| `tp_drv_cst726_reset` | `0x1BD47A` | `0x00DBD47A` | Toggles TP reset GPIO + 100 ms delay |
| `tp_sleep` | `0x1BD4A0` | `0x00DBD4A0` | Writes I2C `0x38` reg `0xA5 = 0x03` |
| `drv_tp_open` | `0x1BD510` | `0x00DBD510` | Initializes CST726 on boot |
| `drv_tp_ioctl` | `0x1BD5A8` | `0x00DBD5A8` | Handles sleep (`0x3E9`) & wake (`0x3EA`) |
| `drv_tp_read` | `0x1BD6B0` | `0x00DBD6B0` | Reads 9 bytes from I2C `0x38` reg `0x00` (240×320) |

---

## 5. Custom Blue iPod Classic Firmware (`firmware/sl6801_ipod.bin`)

Built from [ipod/ipod_ui.c](ipod/ipod_ui.c) + [ipod/build_ipod.py](ipod/build_ipod.py) (compiled via `ziglang` for ARM Cortex-M4F Thumb-2 and injected into the 128 KB free cave at `0x019000`):

- **Power-On Splash Screen:** Replaces the stock `welcome` banner (`0x0FDC8C`) with a tight-cropped **`55×68`** Apple logo ([assets/splash.png](assets/splash.png)) centered on pure `#000000` black.
- **60 FPS Blue iPod Classic UI (v2) + Polar-Angle Touch Click Wheel:** Replaces all 5 music screens (`Music`, `Songs`, `Now Playing`, `Artists`, `Albums`) with an anodized blue iPod shell, crisp white LCD, silver brushed header, and transparent 4× anti-aliased Click Wheel powered by a `16 ms` (60 FPS) timer + Cortex-M4F FPU:
  - **Silky `lv_anim` Page Transitions:** Uses LVGL 8.3's built-in `lv_anim_init` / `lv_anim_start` / `lv_anim_path_ease_out` (`0x00DAE15E`) for smooth `180 ms` horizontal slide transitions.
  - **60 FPS Browse List Physics:** Critically damped spring-interpolated Aqua blue highlight bar (snapping cleanly on page flips) + classic iPod right-edge recessed scrollbar & smooth Aqua thumb + marquee-scrolling selected row.
  - **60 FPS Direct Pixel-Buffer Now Playing:** `62×72` 3D-perspective-tilted Album Art with glossy floor reflection and animated diagonal glass sheen (`rom_sinf`), plus a `208×11` real-time glossy Aqua progress bar with sub-pixel interpolation and animated specular shimmer (`3:41` / `-1:20` timestamps and `6 of 15` track counter).
  - **Baked `INDEXED_4BIT` Sprite Sheet:** Crisp 4× supersampled Play (`▶`) / Pause (`❚❚`) live header status sprites, classic glossy green Battery sprite, and 5 Click Wheel quadrant tactile press glow sprites (`MENU`, `|<<`, `>>|`, `>||`, Center).
- **Authentic Click Wheel Mechanics:** Velocity-accelerated polar scrolling (~26 ticks/rev slow → ~37 medium → ~48 fast); `MENU` goes back, `|<<`/`>>|` skips tracks, Center selects (or opens Settings on Now Playing), and `>||` toggles Play/Pause on Now Playing or plays/enters the highlighted item on browse lists.
- **Click Wheel Audio:** Plays [assets/clickwheel.mp3](assets/clickwheel.mp3) (44.1 kHz soft-limited PCM, `peak=15449`, via CPU0 `ClickTask` hooked at `fw_watchdog_setup` `0x00D4E866`, with 46 ms Class-D amp pre-roll and 1.5 s keep-alive to prevent mid-spin amp pops, releasing `/dev/audio0` immediately on Play/Select).
- **Current Build Checksum:** `firmware/sl6801_ipod.bin` — `FIRM CRC16: 0x04B8` (44.9 KB injected code & assets + 4.7 KB Apple boot logo inside the 128 KB free cave).
- **Touch Sensitivity Patch (`firmware/sl6801_sensitive.bin` & baked into `sl6801_ipod.bin`):**
  1. **`0x1BD560` (`drv_tp_open`):** Writes **I2C `0x38` reg `0x80 = 0x10`** (softer CST726 touch threshold).
  2. **`0x1BD5EC` (`drv_tp_ioctl` wake):** Re-applies `reg 0x80 = 0x10` on every screen wake.
  3. **`0x1ADC94` (`lv_indev_drv_init`):** Increases LVGL `scroll_limit` from `10` px to `20` px.

### Rebuilding
```bash
.venv/bin/python ipod/build_ipod.py
```

### Flashing & Recovery
1. **Enter bootloader mode (`301a:2800`):** Power the player completely off, **hold down the `VOLUME UP` key ONLY** before plugging in USB-C, and keep holding it while plugging in the cable.
2. **Flash:**
   ```bash
   # Flash Blue iPod Classic UI + Click Wheel + Touch Sensitivity patch (recommended)
   sudo ./scripts/flash_ipod.sh

   # Or flash touch sensitivity patch only
   sudo ./scripts/flash_sensitive.sh

   # Or restore untouched stock firmware
   sudo ./scripts/flash_restore.sh
   ```
3. **Power on after reflash (exit ROM bootloader):**
   1. **Unplug the USB-C cable.**
   2. **Hold the Power button for 10 seconds** (forces a hard exit from the ROM bootloader).
   3. **Release** the Power button.
   4. **Press and hold the Power button for 3 seconds (or until it powers on).**

---

## 6. Repository Layout

```
sl6801-ipod/
├── assets/
│   ├── clickwheel.mp3           # iPod Classic click wheel sound effect
│   └── splash.png               # Apple power-on boot logo
├── firmware/
│   ├── sl6801_backup.bin        # Untouched 2 MB stock firmware dump
│   ├── sl6801_sensitive.bin     # Touch-sensitivity patched firmware
│   └── sl6801_ipod.bin          # Full Blue iPod Classic + Click Wheel + Apple splash firmware
├── ipod/
│   ├── fw_240x320.h             # Verified LVGL 8.3 / ROM / OS / Presenter symbol map
│   ├── ipod_ui.c                # Blue iPod Classic UI + Polar Click Wheel + CPU0 ClickTask
│   ├── ipod.ld                  # Linker script targeting 128 KB free cave (0x00C19000)
│   ├── gen_assets.py            # 4x supersampled Click Wheel, Album Art & 44.1 kHz PCM generator
│   └── build_ipod.py            # End-to-end Zig Cortex-M4F build, patch & CRC16 script
├── scripts/
│   ├── patch_sensitivity.py     # Standalone touch-sensitivity patcher
│   ├── flash_ipod.sh            # Flash & verify sl6801_ipod.bin
│   ├── flash_sensitive.sh       # Flash touch-sensitivity patch only
│   └── flash_restore.sh         # Restore & verify untouched stock firmware
├── smartlink_flash/             # ilyakurdyukov/smartlink_flash (built for macOS)
├── reference/
│   └── SL6801_player/           # dbrant/SL6801_player (MIT) tools & SDK reference
└── README.md
```

---

## 7. Credits & References

- **USB Bootloader Flasher:** [`ilyakurdyukov/smartlink_flash`](https://github.com/ilyakurdyukov/smartlink_flash)
- **SL6801 Reverse Engineering & C/LVGL SDK:** [`dbrant/SL6801_player`](https://github.com/dbrant/SL6801_player) (MIT)
- **Community Firmware Reference:** [`zeroproj/OpenPod`](https://github.com/zeroproj/OpenPod)
