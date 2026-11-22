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

## 5. Touch Sensitivity Patch (`firmware/sl6801_sensitive.bin`)

The stock `drv_tp_open` never writes the CST726 touch threshold register (`0x80` / `TH_GROUP`), leaving the screen stiff.

`scripts/patch_sensitivity.py` applies 3 surgical patches (only 3 × 4 KB sectors modified):
1. **`0x1BD560` (`drv_tp_open`):** Replaces unused debug `printf` calls with `tp_set_sensitivity()`, writing **I2C `0x38` reg `0x80 = 0x10`** (softer touch threshold).
2. **`0x1BD5EC` (`drv_tp_ioctl` wake):** Calls `tp_set_sensitivity()` after every screen wake reset.
3. **`0x1ADC94` (`lv_indev_drv_init`):** Increases LVGL `scroll_limit` from `10` px to `20` px so light taps with tiny finger jitter register as clicks instead of scrolls.
4. **`0x00D01C`:** Updates `FIRM` CRC16 (`0xC56E` → `0x39F9`).

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
├── firmware/
│   ├── sl6801_backup.bin        # Untouched 2 MB stock firmware dump
│   └── sl6801_sensitive.bin     # Touch-sensitivity patched firmware
├── scripts/
│   ├── patch_sensitivity.py     # Reproducible touch-sensitivity patcher
│   ├── flash_sensitive.sh       # Flash only the 3 patched sectors
│   └── flash_restore.sh         # Restore stock firmware
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
