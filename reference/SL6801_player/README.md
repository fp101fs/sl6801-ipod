# SL6801 MP3 player: firmware reverse engineering and custom apps

Tools and notes for modifying the firmware of a cheap AliExpress MP3 player with a 1.8" 128x160 color
screen, built around a chip labeled "Jointbees MP3 R14181B6A0" (really a Smartlink **SL6801**).
The vendor firmware is version `yp3_2.0.43`.

|  |  |  |  |
| --- | --- | --- | --- |
|![Album art](./screenshot1.jpg)|![Flappy Bird clone](./screenshot2.jpg)|![Tetris clone](./screenshot3.jpg)|![Mandelbrot viewer](./screenshot4.jpg)|

**Important**: This is only verified to work with the exact model device that I own. YMMV. If you have a "similar" device, but not exactly the same model, I would recommend telling your AI agent about this repo and asking it to use the _architectural_ findings in this repo, and apply the same kind of reverse-engineering on your device's firmware.

What works so far, all tested on the device:

- **Assets**: unpack and repack images, UI strings (10 languages), fonts and the alarm tone, with
  all checksums fixed. Edited data goes to free flash.
- **Custom code**: C apps are compiled to run in place from free flash and call the firmware's own
  LVGL and UI functions. A patched call adds their rows to the firmware's "APP" menu.
- **Apps**!
  - *Flutter*: a side-scrolling Flappy Bird style game.
  - *Mp3tris*: a Tetris clone.
  - *Mandelbrot*: a fractal explorer.
  - *FPU test*: rotating wireframes and an FPU task-switch check.
  - *Key test*: shows key codes.
  - *Sound test*: exercises audio functions.
  - *Mic scope*: an oscilloscope view of the microphone.
  - *SD map* / *Flash map*: the SD card's or flash's sectors colored by entropy.
  - *Task monitor*: FreeRTOS tasks, CPU use and heap.
  - *Benchmark*: CPU clock, memory, flash and math speed.
  - *Pin monitor*: live GPIO levels.
  - *Files*: an SD card browser with a hex and text viewer.
  - *Crash log*: the black box that writes crash and hang reports to the SD card.
- **Album art**: the music player shows the playing MP3's embedded cover behind its "now
  playing" page.
- **Flashing from Windows** over USB with `smtlink_dump`.
- **Ghidra** project setup with ~350 firmware functions named automatically and 100 boot ROM
  functions named by hand, plus headless decompilation scripts.

This repo depends on the original firmware being dumped in the current directory, and called
`firmware_2M.bin`. Please dump the firmware on your own local setup, using the dumping instructions
below. This must remain the original 2 MB flash dump of this unit. Keep it unmodified: every image is
built from it.

`out.bin` is the new firmware, with all the enhancements and apps.

## Contents

- [Quick start](#quick-start)
- [Repository layout](#repository-layout)
- [Workflows](#workflows): [assets](#editing-assets), [apps](#building-apps),
  [flashing](#flashing), [writing an app](#writing-a-new-app),
  [finding firmware functions](#finding-firmware-functions)
- [Reference](#reference): [hardware](#hardware), [memory map](#memory-map),
  [flash layout](#flash-layout), [boot ROM](#boot-rom), [software stack](#software-stack),
  [assets](#assets-inside-firm), [two cores](#two-cores), [SDK internals](#sdk-internals), [audio](#audio),
  [album art](#album-art),
  [recording](#recording), [SD card](#sd-card), [files](#files),
  [diagnostic apps](#diagnostic-apps), [buttons](#buttons-and-key-events), [Ghidra](#ghidra),
  [Windows flashing setup](#windows-flashing-setup)
- [Gotchas](#gotchas) and [open questions](#open-questions)

## Quick start

The environment is Windows with Git Bash; paths below are the ones used here.

| Need | Used here |
|---|---|
| Python 3 venv in `.venv` with Pillow (`ziglang` optional) | `python -m venv .venv` then `.venv/Scripts/python -m pip install -r requirements.txt` |
| Arm GCC | xPack arm-none-eabi-gcc 15.2.1 in `C:/Tools/arm-gnu-toolchain` (or set `ARM_GCC_BIN` to its `bin`) |
| Flasher | `smartlink_flash/smtlink_dump.exe`, built in MSYS2 (see [Windows flashing setup](#windows-flashing-setup)) |
| USB driver | WinUSB for `301A:2800`, installed with Zadig |
| Ghidra (optional) | `C:/Tools/ghidra` (or `GHIDRA_HOME`), JDK via `JAVA_HOME` |

Build the apps, pack them with the edited assets in `work/`, and flash:

```sh
.venv/Scripts/python sdk/build.py sdk/apps/flutter sdk/apps/mp3tris sdk/apps/mandel sdk/apps/fputest sdk/apps/keytest sdk/apps/soundtest sdk/apps/micscope sdk/apps/blockmap sdk/apps/taskmon sdk/apps/bench sdk/apps/pinmon sdk/apps/files sdk/apps/crashlog sdk/apps/albumart
.venv/Scripts/python tools/assets.py pack firmware_2M.bin work out.bin --app sdk/build/app.json
# put the player in bootloader mode, then:
smartlink_flash/smtlink_dump.exe init write_flash 0 0 0 out.bin
```

The new rows appear under **Apps** on the home screen, below Alarm and Pictures.

Before the first write to a new unit, read its flash memory:
`smartlink_flash/smtlink_dump.exe init flash_id read_flash 0 2M firmware_2M.bin`.
To restore the stock firmware, flash `firmware_2M.bin` the same way.

## Repository layout

| Path | What |
|---|---|
| `firmware_2M.bin` | Original flash dump (must not be modified) |
| `bootrom.bin` | Boot ROM dump (optional, for Ghidra; see [Boot ROM](#boot-rom)) |
| `work/` | Unpacked assets (`assets.py unpack`), including a custom 96x96 welcome banner |
| `tools/slfw.py` | Flash image parser: layout, partitions, checksums |
| `tools/assets.py` | Asset unpack/pack/preview, and app injection (`pack --app`) |
| `tools/img2c.py` | PNG to `lv_img_dsc_t` C header (used by the SDK build) |
| `tools/xref.py` | Finds BL/B.W calls to given addresses, or (`--data`) code that loads a given pointer such as a string |
| `tools/avicheck.py` | Checks whether an `.avi` file meets the firmware video player's requirements |
| `tools/find_lv_images.py`, `find_lv_fonts.py`, `render.py`, `findbase.py`, `survey.py` | Analysis helpers used while mapping the firmware (each takes the dump as first argument) |
| `sdk/include/fw.h` | Firmware API: LVGL and vendor functions by address, types, constants |
| `sdk/include/sdk.h`, `sdk/src/` | Shared app code: APP menu hook, full-screen overlay, CPU0 service and workers, black box, sound and mic, helpers, libc and compiler helpers (ROM) |
| `sdk/app.ld`, `sdk/build.py`, `sdk/patches.json` | Linker script, build script, firmware call sites to patch |
| `sdk/apps/flutter`, `mp3tris`, `mandel`, `fputest`, `keytest`, `soundtest`, `micscope`, `blockmap`, `taskmon`, `bench`, `pinmon`, `files`, `crashlog` | Apps |
| `sdk/apps/albumart` | Album art behind the music player (firmware patches only, no menu row) |
| `sdk/test/sfx_preview.c` | Renders an app's sound effects to a WAV file on the PC |
| `ghidra/` | Headless Ghidra import and decompile scripts (`project/`, `input/`, `out/` are generated) |
| `smartlink_flash/` | Local clone and build of the flasher (not in git) |

## Workflows

All Python tools run with the project venv, e.g. `.venv/Scripts/python -I tools/assets.py ...`.

### Editing assets

```sh
.venv/Scripts/python -I tools/assets.py unpack firmware_2M.bin work   # only needed once
# edit files in work/
.venv/Scripts/python -I tools/assets.py pack firmware_2M.bin work out.bin
```

`pack` applies everything that differs from the original, fixes all checksums, prints the changed
flash ranges and the command to flash the result.

- **Strings**: edit `work/strings.json` (10 languages x 219 strings). New text is stored in free
  flash and the string table is repointed.
- **Images**: edit the PNGs in `work/images/`, keeping the file name. A same-size image is
  replaced in place. A new size is stored in free flash and the descriptor updated, but the UI
  layout code may still assume the old size (the welcome banner `img_a5ff8_84x20.png` works fine
  at 96x96). Pixels with alpha < 128 become transparent in chroma-keyed images.
- **Fonts**: set `"rebuild"` in `work/fonts/<font>.json`, e.g.
  `{"ttf": "C:/Windows/Fonts/arial.ttf", "size": 12, "mode": "add_missing", "add_chars": "ŹŻ"}`.
  `add_missing` renders only codepoints the font lacks (for the UI font this includes, by default,
  every character used in `strings.json`); `replace` re-renders every glyph the TTF has. Optional
  keys: `y_shift`, `line_height`, `base_line`, `include_strings`. New glyph bitmaps are appended in
  free flash; the original bitmaps stay where they are.
- **Tone**: replace `work/tone/alarm1.mp3`.

`assets.py preview <image.bin> font_464b0 out.png "text" ...` renders text with a firmware font
(using LVGL's glyph placement) to check font edits before flashing.

`slfw.py <image> info` prints the layout and verifies all checksums; `slfw.py <image> extract <dir>`
writes the partitions; `slfw.py <image> fixcrc <out>` recomputes the checksums.

### Building apps

```sh
.venv/Scripts/python sdk/build.py [--zig] [-o OUT] <app dir>...
```

- Links one or more apps into one image. Their menu rows appear in the order the apps are given,
  e.g. `sdk/build.py sdk/apps/flutter sdk/apps/mp3tris` for just the two games.
- Uses arm-none-eabi-gcc by default, or `zig cc` (from the `ziglang` pip package) with `--zig`.
  Both produce Thumb-2 code for the Cortex-M4F with the hard-float ABI, like the firmware's.
- 64-bit division and most `double` helpers jump to the ROM (`sdk/src/aeabi.c`). With gcc,
  libgcc supplies the rest (`double` comparisons, `double` to `int` and to `float`). zig links no
  runtime library, so code that needs those doesn't link with `--zig`.
- Each app directory's `*.c` files are compiled separately with the app's own include path. Each
  `assets/X.png` becomes a header `X.h` defining an `lv_img_dsc_t` named `img_X`, visible to that
  app only.
- `sdk/src/*.c` is linked into every image.
- Output in `sdk/build/` (default): `app.bin`, `app.json`, and with gcc `app.map` and `app.lst`
  (disassembly interleaved with source, useful for checking firmware addresses).

`app.json` holds the load address, flash offset and the BL patches. Pass it to
`assets.py pack ... --app sdk/build/app.json`. The pack step:
- checks that the target flash region is erased in the original dump;
- checks that each patched BL still calls the expected original function;
- writes the code and the patched BLs.

### Flashing

1. Put the player in bootloader mode: turn it off, then hold the boot key (the Left key on my device)
while connecting USB. It enumerates as `301A:2800`.
2. Run `smartlink_flash/smtlink_dump.exe init write_flash 0 0 0 out.bin`.

Flash the whole image with `write_flash 0 0 0 <file>`. Only sectors that differ are rewritten,
so this is fast.

The USB bootloader is in the chip's boot ROM, so a bad flash can always be recovered by
flashing a good image again. This has happened once and was recovered this way.

### Writing a new app

Create `sdk/apps/<name>/main.c`. A minimal full-screen app that closes on Back:

```c
#include "sdk.h"

typedef struct {
	fw_overlay_t *ov;
} app_t;

static void on_delete(void *ctx)
{
	lv_mem_free(ctx); /* runs however the overlay goes away; stop timers here too */
}

static void key_cb(lv_event_t *e)
{
	app_t *app = lv_event_get_user_data(e);
	uint32_t key = *(uint32_t *)lv_event_get_param(e);
	if (key == FW_KEY_BACK || key == LV_KEY_ESC)
		fw_overlay_close(app->ov); /* frees app via on_delete */
}

static void open(lv_obj_t *return_focus)
{
	app_t *app = lv_mem_alloc(sizeof(app_t));
	if (!app)
		return;
	app->ov = fw_overlay_open(return_focus, lv_rgb(0x10, 0x20, 0x60), on_delete, app);
	if (!app->ov) {
		lv_mem_free(app);
		return;
	}
	lv_obj_align(fw_label(app->ov->root, "Hello", lv_rgb(0xFF, 0xFF, 0xFF)), LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_event_cb(app->ov->root, key_cb, LV_EVENT_KEY, app);
}

FW_MENU_ITEM(FW_SYMBOL_OK, "Hello", open);
```

Rules and tips:

- **UI code runs on the second core.** Everything an app does in its menu row, key and delete
  callbacks and LVGL timers runs on CPU1, while FreeRTOS runs on CPU0 (see
  [Two cores](#two-cores)). Never call the kernel from there: no `fw_os_*`, `fw_task_*`,
  `fw_sem_*`, `rom_task_states`, critical sections, FatFs or `fw_dev_*`. Doing so freezes the
  player at random, mostly during music playback. Use a worker instead.
- **No writable globals.** Code runs from flash; the linker script rejects `.data` and `.bss`.
  Keep all state in memory from `lv_mem_alloc` and free it in `on_delete`. `static const` tables
  are fine.
- **Memory is tight.** The LVGL heap is 40 KB for the whole UI. Prefer a few objects; for many
  small elements draw into an indexed image instead (see Mp3tris).
  - Put buffers of more than a few KB, such as image pixels, in a worker's memory on the
    firmware heap: `fw_worker_start(NULL, size, NULL, 0, 0)` allocates without a task (see Pin
    monitor).
  - A large `lv_mem_alloc` can fail when the UI heap is fragmented, and then the app silently
    doesn't open. One that barely succeeds leaves the firmware's own UI short of memory, which
    can crash it later.
- **Animation**: use `lv_timer_create` and `lv_tick_get`; see Flutter for a fixed-step loop that
  catches up when the timer fires late.
- **Heavy computation, files and devices**: run them in a worker task, which shares the worker's
  memory with the UI, and draw its results from a UI timer; see Mandelbrot and Files.
- **Math**: `float` runs on the FPU (single precision). `double` is emulated in software and much
  slower. `rom_sinf`, `rom_cosf`, `rom_log2f` and `rom_exp2f` are fast table lookups; see FPU
  test. Mandelbrot uses 32-bit fixed point instead, which has more precision than `float` for
  deep zooms.
- **Timing**: `rom_timer_read64()` counts at `FW_TIMER_HZ`, for measuring code to the
  microsecond (FPU test shows its frame time this way).
- **Input**: see [Buttons and key events](#buttons-and-key-events). Center needs special handling
  because its `LV_EVENT_KEY` only arrives on release.
- **Menu icons**: `FW_FONT_ICONS` has only these FontAwesome codepoints: F001 F008 F00C F013 F01D
  F028 F02D F02E F04A F04B F04C F04E F095 F09B F0C7 F0C9 F115 F130 F131 F1C5 F21D F260 F28C F294.
  `sdk.h` defines `FW_SYMBOL_AUDIO`, `OK`, `PLAY` and `BARS`.
- **Sprites**: put PNGs in `assets/`; `#include "X.h"` and use `&img_X` with `lv_img_set_src`.
  Use hard alpha (the firmware draws chroma-keyed images, not alpha blending). Flutter's `art.py`
  shows how to draw sprites with Pillow.
- **Sound effects**: see [Audio](#audio). The usual pattern:
  - `fw_sound_open()` when the app opens;
  - `fw_sound_play(snd, FW_SFX(table))` for each effect;
  - `fw_sound_close()` in `on_delete`.

  Effects are tables of `fw_tone_t`. Both games keep theirs in `sfx.h`. To listen to them on the
  PC, build `sdk/test/sfx_preview.c` (instructions at its top), which writes all of an app's
  effects to a WAV file.
- **Microphone**: `fw_mic_open()` when the app opens, `fw_mic_latest(mic, buf, frames)` from a
  timer to get the newest stereo samples, `fw_mic_close()` in `on_delete`. See Mic scope and
  [Recording](#recording).
- **SD card sectors**: `fw_dev_read(FW_SD_NODE, buf, count, &sector)`; see SD map and
  [SD card](#sd-card).
- **Files**: `f_opendir`/`f_readdir` and `f_open`/`f_read`/`f_close` from a task, after checking
  `fw_sd_mounted()`; see the Files app and [Files](#files).
- **Host testing**: game logic can be compiled and run on the PC with MSYS2 gcc. `#undef` the
  `fw.h` macros the app uses, define stubs, then `#include "main.c"`.
  `sdk/apps/mp3tris/test/host_test.c` drives the game with fake events and ticks, and `show.py`
  renders its image dumps to PNG.

### Finding firmware functions

1. Build the Ghidra project once: `ghidra/import.sh`.
2. Look up names in `ghidra/out/sl6801_app.bin_functions.csv`. Functions are named from their
   `__func__` strings, which covers most of LVGL's core and the vendor UI pages.
3. Decompile without the GUI: `ghidra/decompile.sh app out.c <name|0xaddr>...`.
4. Find callers: `.venv/Scripts/python tools/xref.py firmware_2M.bin 0xADDR...`. To find the
   code that uses a string, search the dump for the string's offset and look up its address
   (`0xC00000 + offset`) with `tools/xref.py --data firmware_2M.bin 0xADDR`. Decompiling the
   reported instruction address shows the function.
5. Add the function to `fw.h` as `#define name FW_FN(0xADDR, ret, (args))`. Compare against the
   LVGL v8.3 source to confirm the signature, then check the literal in `sdk/build/app.lst`
   after building.

Unnamed LVGL functions can usually be identified from a named caller. For example,
`lv_img_set_src` calls `lv_obj_invalidate` first and last. Style setters can be identified by
their v8.3 property IDs (e.g. `BG_COLOR` 0x20, `TEXT_COLOR` 0x457, `TEXT_FONT` 0x1459).

## Reference

### Hardware

From photos of the board:

| Part | Identification |
|---|---|
| SoC | "Jointbees MP3 R14181B6A0", QFN-48, really a Smartlink **SL6801** (string `SL6801` in the bootloader). 2 MB flash in the package, no external flash chip. |
| CPU | Two Cortex-M4F cores: CPU0 runs FreeRTOS, CPU1 the UI (see [Two cores](#two-cores)). |
| Display | 128x160 ST7789 TFT on a 20-pin FPC (`st7789s_lcd_init`), RGB565 |
| FM radio | RDA5807M (marked `5807M`) |
| Speaker amp | 8002D (SOP-8, class AB) |
| Other | microSD slot (SDIO), mic (MIC1), PCB trace antenna (Bluetooth), 5 buttons, slide switch, USB-C, 3.5 mm jack, 150 mAh LiPo |
| Test pads | `5V DM DP G ADC` (USB + ADC) next to R27-R30 |

USB IDs: `301A:2800` in bootloader mode (boot ROM, serial `20201111000001`), `301A:2801` as a card
reader in normal operation.

### Memory map

| Address | What |
|---|---|
| `0x00000000` | Boot ROM, contents up to `0x5BEB4` (see [Boot ROM](#boot-rom)) |
| `0x00800000`-`~0x0087FFFF` | RAM. App initial SP `0x0086B710`, bootloader SP `0x0083BE90` |
| `0x00800000`-`0x00804AB0` | Boot ROM `.data` and `.bss`: RAM vector table, hook tables, FreeRTOS state |
| `0x00804800` | App vector table (FIRM RAM block), reset at `0x00804C40` |
| `0x0080CC40`-`0x0081AAEC` | App `.data`, copied from flash `0x39470`-`0x4731C` at startup (RAM = flash offset + `0x7D37D0`) |
| `0x0081AAEC`-`0x0086B710` | App `.bss` (zeroed by Reset_Handler) |
| `0x0081FBC0` | Bootloader vector table (loaded from flash `0x60`), reset at `0x00820000` |
| `0x00876000` | LVGL heap (40 KB TLSF pool) |
| `0x00C00000` | SPI flash, memory mapped (XIP). Flash offset `X` is at `0xC00000 + X` |
| `0x30000000` | Flash controller registers (base of the ROM's flash driver) |
| `0x40000000`-`0x400FFFFF` | Peripheral registers |
| `0xE000E018` | SysTick current value (used as a random seed) |

### Flash layout

| Flash offset | Size | Content |
|---|---|---|
| `0x000000` | 0x60 | `HLKJ` header: +4 load address, +8 entry, +0xC code offset, +0x10 code size, +0x14 CRC16, +0x20 partition table offset, +0x5C CRC16 of the header's first 0x5C bytes |
| `0x000060` | 0xC8EC | Bootloader (FreeRTOS-based; LCD/SD init, firmware update from PC/SD) |
| `0x00D000` | 0x40 | Partition table: u32 count (3), ..., 16-byte entries from +0x10: name, offset, size, CRC16 |
| `0x00E000` | 0x174270 | `FIRM` main application |
| `0x183000` | 0x2038 | `TONE`: u32 size + name (`alarm1.mp3`), data at +0x400 (MP3 with ID3 tag) |
| `0x185038` | ~475 KB | Free (0xFF). Used by these tools: asset data from `0x186000`, app code from `0x1D0000` |
| `0x1FC000` | 0x4000 | `PSMP` settings (non-volatile storage, not checksummed) |

The free area is not touched by the vendor firmware as far as is known, and data placed there
reads back fine at run time.

**FIRM header** (0x30 bytes):

| Offset | Value | Meaning |
|---|---|---|
| +0x00 | 0x30 | header size |
| +0x04 | | build timestamp |
| +0x10 | 0x804C00 | unclear; disassembly shows the block's vectors at 0x804800 |
| +0x14 | 0x804C01 | entry |
| +0x18 | 0x1000 | size of the RAM block (starts at FIRM+0x30) |
| +0x1C | | CRC16 of the RAM block |

The rest of FIRM runs in place from flash. There is a 130 KB zero-filled run at flash
`0x18A8E`-`0x39470`, right before the `.data` load image; its purpose is unknown, so the tools
leave it alone.

**Checksums**: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the bootloader, FIRM, TONE,
the FIRM RAM block and the `HLKJ` header itself. There are no signatures. `slfw.py fixcrc`
reproduces the original dump byte for byte.

### Boot ROM

The chip's mask ROM is mapped at `0x00000000`. To dump it, put the player in bootloader mode and
run `smartlink_flash/smtlink_dump.exe init read_mem 0 0x60000 bootrom.bin`. A larger read works
too, since everything past the ROM reads as zeros. Keep the file as `bootrom.bin` next to the
firmware dump; `ghidra/import.sh` maps it into both programs.

The contents end at `0x5BEB4`, so the ROM is probably 384 KB: code, then constant tables, then
the initial values of its `.data` at `0x5B800`.

**Startup.**
- `rom_reset` (0x40) first checks the core ID at `0x4000000C`. On CPU1 it jumps through the
  word at `0x30000600`, which the firmware sets before starting that core (see
  [Two cores](#two-cores)).
- Otherwise it copies `.data` to `0x800000`-`0x8006B4`, zeroes `.bss` up to `0x804AB0` and calls
  `rom_main` (0x4C9C8).
- `rom_boot` (0x4C914) points the vector table at RAM (`VTOR = 0x800000`), sets up the timer and
  reads the boot key pin (`0x30000`: port 3, pin 0; low when held).
- **Boot key held:** `rom_usb_download` (0x4C8A0) briefly waits for a USB host (`301A:2800`). If
  none answers, it goes on to boot from flash.
- **Flash boot** (`rom_flash_boot` 0x4CD7C):
  1. Reads the 0x5C-byte `HLKJ` header from flash 0 and checks the magic and the CRC16 at +0x5C.
     It tries 3 times, then 3 more times at flash 0x1000 (a backup location this firmware doesn't
     use).
  2. Copies +0x10 bytes from flash +0xC to RAM at +4 in 2 KB reads. The flash controller computes
     the CRC16 in hardware, and the result is compared with +0x14.
  3. Disables interrupts and calls +8.

  If any step fails, it stays in USB download mode for good. That is why a bad flash can always
  be recovered.
- Progress codes go to `0x40000100` along the way: `0x1010`-`0x1055` during the checks, `0x1234`
  just before the jump.

**Contents.** The firmware calls about 300 ROM functions from 6,000 call sites.

| ROM range | What |
|---|---|
| `0x0`-`0x1000` | Debug UART and printf (0x2B0), pin setup and GPIO input (0x7AC, 0x7EC), C library (`memcpy` 0xB2A, `memset` 0xBAA, `memmove`, `memcmp`, `strlen`, `strcmp`, `strcpy`, `atoi`, `qsort`) |
| `0x1000`-`0x2000` | PCM sample conversions; compiler helpers (`__aeabi_dadd` and the other double ops, `__aeabi_ldivmod`/`uldivmod`) |
| `0x2000`-`0xC000` | Clock gates (`rom_clk_enable` 0x2564), SPI flash, timer delays, FreeRTOS kernel and heap (`xTaskCreate` 0x8A30, `pvPortMalloc` 0x9D50, queues, event groups) |
| `0x10000`-`0x20000` | Audio decoding in float: an MP4/M4A atom parser (0x1BED0 holds `alac`, `mp4a`, `esds`), an MP3 Xing header parser (0x1D360) |
| `0x20000`-`0x42000` | Bluetooth (called from the firmware's `hci_*` functions) |
| `0x45000`-`0x5B800` | WAV decoders: a codec table by format tag at 0x59600 (PCM, MS ADPCM, float, A-law, µ-law, IMA ADPCM, ...). Also a file type table at 0x59680 (WAVE, the ASF header GUID, OggS, fLaC, ftyp, MAC, RIFF), table-based `sinf`/`cosf`/`log2f`/`exp2f`, filter tables, and the boot code |

**RAM.** The ROM's `.data` and `.bss` are at `0x800000`-`0x804AB0` and contain:
- the RAM vector table;
- the printf output hooks at `0x80082C`, set by `rom_printf_init` (0x2A0);
- a memory pool for the audio code and its hook table at `0x800988`;
- another alloc/free hook pair at `0x8009CC`;
- the FreeRTOS kernel's state.

The firmware leaves this area alone. Its own RAM starts at `0x804800`; the ROM's `.bss` above that
is only used during boot.

**Using it.**
- `fw.h` binds the C library, `rom_tick_count` (FreeRTOS ticks at 1 kHz, i.e. milliseconds),
  busy-wait delays, critical sections, clock gates, GPIO input, mono-to-stereo PCM expansion,
  and the float approximations.
- The SDK's `memset`, `memcpy`, `memmove` and `strlen` forward to the ROM, and so do the
  compiler's 64-bit division and `double` helpers (`sdk/src/aeabi.c`).
- The ROM's helpers match GCC 15's libgcc byte for byte, except that its `double` add/subtract
  predates a libgcc fix for results at the edge of the denormal range.
- All the names are in `ROM_FUNCTIONS` in `ghidra/scripts/SL6801Setup.java`.

**FPU.** The CPU has a single-precision FPU (Cortex-M4F, FPv4-SP), and it is already enabled when
apps run. The ROM's `xPortPendSVHandler` saves FPU registers on task switches. The ROM, the
firmware and the SDK all use the hard-float ABI (float arguments in `s0`-`s15`).

### Software stack

From strings and decompilation:

- FreeRTOS (the kernel is in the [boot ROM](#boot-rom)) with an OAL layer and Linux-style device
  nodes: `/dev/lcd`, `/dev/audio0` (play),
  `/dev/audio1` (record), `/dev/fm`, `/dev/tp`, `/dev/usbd`, `/dev/uart0..2`, `/dev/key_io`,
  `/dev/key_onoff`, `/dev/kadc_ch1`, `/dev/timer`.
- LVGL v8.3 (source path `F:\pen133_yp3_mp\spark2\src\gui8\lvgl\...`), configured with
  `LV_COLOR_16_SWAP=1`, `LV_USE_USER_DATA=0`, 16-bit `lv_coord_t`, `LV_FONT_FMT_TXT_LARGE`, and
  the image cache disabled. Asserts include function names, which helps naming functions.
- FatFs (R0.14 style) with exFAT enabled:
  - `check_fs` 0xD5609C recognizes the exFAT boot sector. The volume mount 0xD573E8 has the full
    exFAT branch: version 1.00, 512-byte sectors, one FAT, allocation bitmap. `f_mkfs` 0xD58F38
    can create exFAT.
  - Partitions: an unpartitioned card or the 4 MBR primary partitions; no GPT handling was found.
  - The SD driver's ioctl 3 (0xD46EB4) reads the boot sector and reports the volume's "dirty"
    flag (FAT32 byte 0x41, exFAT VolumeFlags byte 0x6A). For a partitioned exFAT card it returns
    1 when the flag is set. Who uses that result is not known yet.
  - The bootloader has its own FatFs copy with the same exFAT signature check.
- libjpeg, codecs: MP3, FLAC, APE, WMA, AAC, OGG/Vorbis, SBC/mSBC.
- Bluetooth classic: A2DP source and sink, AVRCP, HFP.
- Apps: music (database with artist/album/favorites, `.lrc` lyrics), video (MJPEG AVI), photo
  viewer, ebook, English dictionary, voice recorder (WAV/ADPCM to `0:\record`), FM radio, alarm,
  EQ.
- UI pages are created by `view_page_create` (0xD04A1C), a switch on page id; page handlers are
  `pageN_*_process` functions.

### Assets inside FIRM

**Images**: 15 LVGL `lv_img_dsc_t` (`{u32 header (cf:5, w:11, h:11), u32 data_size, u8 *data}`),
each followed by its pixels, around flash `0x9D1E8`-`0xA6D24`: 9 menu icons 29x29, 4 app icons
50x50 (3 of them 8-bit indexed), one 16x16, and the 84x20 "welcome" banner. True color is RGB565
with bytes swapped. Chroma-keyed images use pure green `0x07E0` as transparent. Most other UI
graphics are glyphs from icon fonts.

**UI strings**: `const char *` table at flash `0x53154`: 10 languages x 219 UTF-8 strings (index 0
is the language's own name). Order: de, en, fr, it, ja, nl, pl, pt, ro, es. A 10-entry array of
per-language table pointers is at `0x48770` (copies at `0x48798` and `0x101FE8`).
`get_lang_str(id)` (0xD01F8C) returns a string in the current language.

**Fonts**: 16-byte glyph descriptors
(`u32 bitmap_index, u32 adv_w, u16 box_w, u16 box_h, i16 ofs_x, i16 ofs_y`), glyph bitmaps packed
continuously, MSB first. Two kinds:

- "sl" fonts (9, all in `.data`): custom per-font `get_glyph_dsc`/`get_glyph_bitmap` functions
  (e.g. `0xD055BB`/`0xD05579`). A single cmap: `range_start` = first code, `range_length` = last
  code, `unicode_list` = sorted absolute u16 codes, binary searched; glyph id = index in that list.
  `adv_w` is in whole pixels. Layout in `.data`: dsc (0x18) + glyph cache (8) + `lv_font_t` (0x18).
  - `font_464b0`: the UI text font (`lv_font_t` at RAM `0x819C98`), 1 bpp, line height 15, 3237
    glyphs (Latin, Latin-1, some Latin Extended, Hebrew, kana, ~2400 CJK). Missing glyphs used by
    the strings: Polish/Romanian letters (ă ł ț ș ć ś ń ż ź Ł Ś Ș ţ), ¡ ¿ °, and 10 CJK
    characters. These render as gaps unless added with a font rebuild.
  - `font_46440`, `font_46470`: clock digits; the others are FontAwesome-style icon fonts.
- Standard `lv_font_fmt_txt` fonts in flash (`get_glyph_dsc` = `0xD374ED`): small icon fonts in
  the Private Use Area (U+E6xx, iconfont.cn) and a few digit/ASCII fonts, e.g. `0xC610F8`
  (23 px clock digits, only `/0123456789:CELMOW`). The asset tools don't handle these yet.

**Other data**: Unicode/GBK conversion table around `0xB8000`-`0xC4000` (FatFs code page); the
`TONE` partition holds the alarm MP3.

### Two cores

The SoC has two Cortex-M4F cores sharing RAM, flash and peripherals; `0x4000000C` bit 0 reads 0
on CPU0 and 1 on CPU1 (`FW_CORE_ID`).
- **CPU0** boots, runs FreeRTOS and every task: the music player, the card, the drivers, the
  firmware heap. The tasks in the Task monitor are all CPU0's.
- **CPU1** runs the UI. `FUN_00CD85A0` starts it through `FUN_00CD6968`, which writes a
  trampoline (0xCD6931) to `0x30000600`, the core's stack pointer to `0x30000604` and its entry
  (0x80DA4F, in RAM) to `0x30000608`, then cycles clock 0. The ROM's reset sends CPU1 through the
  trampoline, which loads that stack, enables the FPU and jumps to the entry.
- **The UI loop** (`watch_view_task`, "ViewTask", 0xCD8FE0: LVGL, the firmware's pages, all app
  UI code) runs on CPU1 on its own stack (around 0x81ABC8-0x81BBD4). It talks to the presenter
  tasks on CPU0 only through message rings (`FUN_00D043CC` sends, `FUN_00CD9214` receives), and
  allocates only from LVGL's heap. CPU1 also reads CPU0's `pxCurrentTCB`, which is why the UI
  seems to run "in" whichever task CPU0 is running.
- The fault vector table at 0x800000 is shared: a fault on either core goes through it.

**Kernel calls from CPU1 corrupt CPU0's state.** FreeRTOS protects its heap and lists by
suspending the scheduler or masking interrupts, which only holds off code on CPU0. A heap or
task call from the UI runs alongside CPU0's own calls and now and then corrupts the heap or the
ready lists. The player then freezes (often with every task's CPU use stuck at 0), and the
firmware's watchdog resets it about 15 s later. CPU0 is busiest during music playback, so that
is when it happens. Early symptoms that were blamed on other causes: devices opened, written or
closed from UI timers hung or rebooted the player, SD reads from a UI timer rebooted it, and a
sleep from the UI froze it for good.

**Service and workers** (`sdk/src/service.c`). The SDK replaces the `bl 0xD27AF0` (the watchdog
setup) at `0xCD8C7A`, which the firmware's first task makes once at startup on CPU0, with
`hook_cpu0_start`. It calls the original, allocates the state both cores share (its address
goes in the reserved vector table entry at 0x80001C, as app code has no writable globals) and
starts the `sdk` service task at priority 18.
- The UI claims one of `FW_WORKERS` slots with `fw_worker_start(entry, mem_size, name, priority,
  stack_words)`, which returns at once.
- The service polls the slots every 5 ms. For a new one it allocates the zeroed memory on the
  firmware heap (keeping 32 KB free for the firmware), creates the task if there is an entry, and
  sets the state to RUNNING, or FAILED.
- The UI reads the state and `fw_worker_mem` from a timer. It shares data with the task through
  that memory with plain reads and writes; tasks poll for requests with `fw_os_sleep`.
- `fw_worker_release` sets `stop` and hands the slot back; once the entry has returned the
  service joins the task, frees the memory and frees the slot.

**Black box** (`sdk/src/blackbox.c`). RAM does not survive the watchdog's reset (it goes through
the power management chip: `FUN_0080CF9E` sets bit 0x80 of its register 0x20), and the chip
ignores `SYSRESETREQ`. So reports go to the SD card while CPU0 still runs:
- Fault vectors 3-6 point to the black box's handler. It records the registers, fault status
  and stack, then parks a faulting CPU0 task in a sleep loop (by rewriting its stacked pc) so
  the rest keeps running. A fault on CPU1 just spins there.
- The `bl fw_watch_checkin` at `0xCD9074`, the UI loop's watchdog check-in, goes to
  `hook_view_alive`, which timestamps each pass.
- Every 100 ms the service writes `0:/crashlog.txt` once after a fault or when the UI loop has
  stopped for 3 s: the fault, every task's state, CPU use, saved pc and lr, the watchdog
  monitor's bits (0x81D15C) and the heap.
- While the Crash log app has it turned on, it rewrites `0:/bbstatus.txt` every second with the
  same state, for hangs that stop CPU0 too.

**Watchdog.** `watch_watchdog_refresh` (0xD27B78) feeds `/dev/wdog` only when every watched task
has checked in since the last refresh: the UI loop (watch task 3, once per pass) and watch task 9
(once per message). Otherwise it logs "task %d (%s)is dead" and the player resets.

### SDK internals

**Calling the firmware.** `fw.h` declares each function as a macro that casts its address (with the
Thumb bit) to a function pointer, so calls compile to `ldr` + `blx` with no linking against the
firmware. Code is linked at `0xDD0000` (flash `0x1D0000`, up to `0x1FC000`, 176 KB) and runs in
place.

**APP menu hook** (`sdk/src/menu.c`).
- The home grid's "APP" item opens page 0x53, `page_home_menu_create` (0xD0FC64): a list with
  Alarm and Pictures (string ids 7 and 4).
- Its rows are made by `fw_list_item_create` (0xD0267C), each with a FontAwesome symbol label
  (font `0xC810C4`) and a text label. The page state is at `*(0x81CE5C) + 0x1DC`.
- `sdk/patches.json` redirects the `bl page_home_menu_create` at `0xD04C04` in `view_page_create`
  to `hook_home_menu_create`. The hook calls the original, then appends one row per
  `FW_MENU_ITEM`. The build keeps every patch target when it discards unused code.
- The other patches: the key hook (see [Buttons and key events](#buttons-and-key-events)), and
  the service start and UI loop check-in (see [Two cores](#two-cores)).
- The linker script collects the `FW_MENU_ITEM` entries between `__fw_menu_start` and
  `__fw_menu_end`.
- Each row's key handler opens the app on ENTER and passes other keys to
  `page_home_menu_event_cb` (0xD0FB2C), so navigation and Back keep working.
- Page 0x51 (`page_expand_home_create`, a 2x3 icon grid) exists in the code but is not reachable
  on this model.

**Overlays** (`sdk/src/overlay.c`).
- `fw_overlay_open` creates a full-screen object on `lv_layer_top()`, adds it to the view's key
  group (`*(*(0x81CE5C) + 0x30)`), focuses it and freezes the group, so the app gets all key
  events.
- `fw_overlay_close` deletes it and gives focus back to the menu row.
- `on_delete` runs from `LV_EVENT_DELETE`, also if the firmware tears down the UI by itself, so
  it is the one place to stop timers and free memory.

**LVGL heap.** `lv_mem_init` (0xD39014) creates a 40 KB TLSF pool for the whole UI. The image cache
is disabled (`_lv_img_cache_open` decodes on every draw), so an `lv_img_dsc_t` in RAM can be
changed in place and redrawn with `lv_obj_invalidate`. Mp3tris draws its board as a 70x140
`LV_IMG_CF_INDEXED_4BIT` image (16-color palette of `b, g, r, a` entries, then 4-bit pixels with
the first pixel in the high nibble; alpha must be 0xFF).

**Useful addresses** (all in `fw.h`): `lv_mem_alloc` 0xD3909C, `lv_mem_free` 0xD39030,
`lv_obj_create` 0xD2A250, `lv_obj_del` 0xD2E3B4, `lv_obj_invalidate` 0xD2B770, `lv_label_create`
0xD3F1FC, `lv_img_create` 0xD3E764, `lv_img_set_src` 0xD3E77C, `lv_timer_create` 0xD39F34,
`lv_timer_del` 0xD39FA4, `lv_tick_get` 0xD37980, `lv_obj_add_event_cb` 0xD280C8, and the style
setters from 0xD2E074.

### Pages and navigation

- **View side:** `view_page_create` (0xD04A1C) switches on the view page id (list of `page_*_create`
  functions in the Ghidra CSV). View event callbacks report key actions to the logic side with
  `FUN_00D04430(page, action, index, ...)`.
- **Logic side:** the "presenter" `pageN_*` functions decide what happens. They navigate with
  `FUN_00CEEAFC(current, action, index, target_page)` and go back with `FUN_00CEEFE0`.
- **Numbering:** presenter numbers equal view page ids for the low pages (e.g. `page20_*` is
  video play, 0x14). For the highest pages they are one higher: `page81_*` is the password menu
  (0x50), `page82_*` view 0x51, and `page84_*` the APP list (0x53). Where the offset starts
  hasn't been pinned down.
- **Home grid** (page 1, `FUN_00CE1E9C`): Music → 0x02, Video → 0x13, Ebook → 0x0C,
  Recorder → 0x18, FM → 0x1A, APP → 0x53, Folder, Bluetooth → 0x23, Settings → 0x28. Index 10,
  Pictures → 0x15, is handled but has no icon; Pictures moved into APP.
- **APP list** (0x53): Alarm → 0x1E, Pictures → 0x15.
- **Settings** (0x28): Language 0x29, Date/time 0x2A, Speaker 0x4C, Brightness 0x2D, Screen off
  0x2E, Auto power-off 0x30, Sleep timer 0x4D, Device info 0x33, Storage 0x4E, then the actions
  Restore factory settings, Update Playlist, and Format SD card.

### Hidden features

These pages are complete in the firmware, but no menu navigates to them on this model:

- **Expand home** (0x51): a 2x3 icon grid with 4 icons: Pictures, Alarm, FM, and "Aa" with a
  magnifier (dictionary). Its parts don't agree with each other:
  - the label list is Ebook, Pictures, FM, Recorder, Settings;
  - its presenter (`page82_img_process`, 0xCED5AC) sends the slots to Video, Pictures, Ebook,
    Recorder, FM, Settings and Music.
  It looks like an abandoned layout from another product variant.
- **Dictionary** (0x37 `page_dict_input` keyboard entry, 0x38 `page_dict_play` result with a
  pronunciation "Play" button):
  - Data comes from `0:\dict` ("No Dictionary Data Detected" otherwise).
  - Lookup `FUN_00CE0C20` binary-searches an index of 48-byte records: a 32-byte word, then
    offset/length into a text file and into an audio file (up to 80 KB per word). The first 76
    records are cached in RAM.
  - The data file names have not been found yet.
  - The navigation code has a case for entering 0x37 (`pstr_enter_page56`), but nothing calls
    it with 0x37. The word display page also special-cases a few words ("eat", "keep", "know",
    "spend", "dead", ...).
- **Power-on password** (0x50 `page_pass_menu`: "Power-on password" on/off and "Change
  password"; 0x4F `page_pass_input`): stored in NV settings (`pstr_pass_nv_save`). The menu itself
  is not linked from Settings.
- **Key light** (0x4B `page_set_keylight`, string "Key light"): probably for models with
  backlit keys.

Showing any of these needs a logic-side change: an APP row's ENTER handler (view side) can't
navigate on its own. The APP presenter (`page84_*`, 0xCEDA8C) ignores indexes above 1.

### Video player

The home screen's "Video" item plays only one specific format, so most video files are ignored
or rejected. The rules below come from the decompiled scanner (0xD53AD8) and parser
(0xCF2DBC, 0xCF3500), and `tools/avicheck.py` applies the same checks to a file.

**Finding files.**
- The media database scan (`0:\DBROOT\db_video.db`) lists files by extension only, and the only
  video extension is `.avi` (case-insensitive).
- It searches the whole card up to 9 folders deep and lists at most 5000 files.
- It skips `0:\record`, `0:\DBROOT`, `0:\dict` and `System Volume Information`.

**Playing them.** A listed file plays only if:
- It is a RIFF AVI with `hdrl`/`avih` first, followed directly by the stream lists. The video
  stream is `vids` with codec `MJPG` (each frame a baseline JPEG, decoded with libjpeg).
- The video is **exactly 128x160** (the screen in portrait); any other size fails with "width and
  height failed".
- Any audio track is **stereo** and is played as 16-bit PCM.
- `movi` starts in the first 30 KB, and every chunk in it is a video or audio chunk of at most
  30 KB, ending at `idx1`.
- The frame rate comes from `avih`.

**The player mirrors the picture horizontally.** A file that looks right on a PC shows up
left-right flipped on the device; this was seen on the player itself, and the cause in the
firmware hasn't been found. To compensate, mirror the video during conversion with `hflip`
(the commands below include it). Remove `hflip` if your player shows the video the right way
round without it.

ffmpeg's AVI output meets these rules. To convert a video for portrait viewing, letterboxed:

```sh
ffmpeg -i in.mp4 -vf "scale=128:160:force_original_aspect_ratio=decrease,pad=128:160:(ow-iw)/2:(oh-ih)/2,hflip,fps=20" \
  -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a pcm_s16le -ar 44100 -ac 2 out.avi
```

To watch with the player turned sideways, scale to 160x128, rotate, then mirror:
`-vf "scale=160:128:force_original_aspect_ratio=decrease,pad=160:128:(ow-iw)/2:(oh-ih)/2,transpose=1,hflip,fps=20"`
(use `transpose=2` to rotate the other way). `hflip` must come after `transpose`, because the
device mirrors the picture as it appears on the screen.

Not yet confirmed on the device: what frame rate the decoder keeps up with, and when the
database is rescanned after files are copied to the card.

### Audio

**Device layer.** Drivers are nodes in a list at 0x81D40C. The calls are `fw_dev_open(name,
flags, params)` (0xD44604), `fw_dev_close(&node)` (0xD44674) and `fw_dev_write(node, buf, count,
0)` (0xD446BE). A device's driver functions are read, write, event, open and close at the start
of the object its probe function returns.
- Opening a device that is already open returns the same node without reopening it. Closing that
  node would close the other user's stream, so the SDK checks the node's "open" flag first
  (`fw_dev_busy`) and does nothing if the device is busy.

**`/dev/audio0`** (driver functions: open 0xD46434, write 0xD4628C, close 0xD462BC) takes 10
words of open parameters:
- channels, bytes per sample, sample rate, period (frames), periods in the ring buffer,
  start threshold (frames), 0, 0, event callback, callback argument;
- rates of 16 kHz or less fail, and mono is converted to stereo;
- every write must be exactly one period;
- a start threshold of 0 means the whole ring must fill before output starts.

The ring is written by `FUN_00813982`, which waits (for up to 3 s) only when the ring is full.
The video player (`FUN_00CF3500`) and the music player's sink (`FUN_00D4968C`) both use this
device.

The event callback gets (arg, 1) after open and (arg, 2) on close. The video player's callback
(0xCF34C5) switches the speaker amplifier ("PA") on through a boot-ROM routine when the Speaker
setting is on, and off again on close.

**How the firmware's players use it.** Both feed the device from a dedicated task, keep the ring
full with blocking writes, and close only after stopping that task. The video player
(`FUN_00CF3500`, close `FUN_00CF3744`) opens a stream of 256-frame periods × 16 with its PA
callback and runs `FUN_00CF3324` in a task. The music player's sink is `FUN_00D4968C`.

**Closing safely.** The playback DMA interrupt (0x8167E8) keeps a state byte at +0x21 of the
hardware object that `*(void **)0x81E320` points to: 0 idle, 1 priming (it loops 16 bytes of
silence), 2 playing, 3 starved. While playing, each interrupt consumes the finished period and
starts the next one from the ring, and switches to "starved" when less than a period is left.
Closing the device stops the stream (which starts one more period from the ring), releases the
DMA channel and frees the ring, but does not stop the interrupt. If the ring still holds data,
the next interrupt starts a transfer from the freed ring with the cleared channel handle. When the
SDK closed with a full ring, opening the audio device after the resulting reboot rebooted the
player again until a power cycle; waiting for the ring to run dry first stopped that. So close
only once the ring has run dry (state 3). The video player gets this for free, because
it frees its buffers and closes the file between stopping its task and closing the device.

**Don't touch `/dev/audio0` from UI code.** The UI loop, `watch_view_task` (0xCD8FE0), calls
`lv_timer_handler` (0xD39FC8) in a loop on CPU1; app timers and key events run inside it.
Measured with an earlier version of the Sound test app, which ran each device call from a UI
timer and logged it on screen first:
- a write from the UI that has to wait for ring space hangs for about 10 s and then reboots,
  every time;
- opening or closing from the UI sometimes crashes, even with nothing played;
- joining the sound task, or sleeping while the ring drains, from the UI hung on exit.

The same writes from a task always worked: the drivers and the kernel belong to CPU0 (see
[Two cores](#two-cores)). The music player opens, writes and closes from its own task. The video
player opens and closes from presenter code on CPU0. The Sound test app (`sdk/apps/soundtest`)
now only checks the SDK sound module: it opens it, beeps and closes it three times, logging each
step.

**Volume.** `fw_audio_volume()` (0xCDF884) returns the user's volume in the low byte. The video
player scales samples with `fw_gain_apply(gain, volume, buf, frames)` (0xD47C10) on a gain object
from `fw_gain_create(0, rate, 2, 2)`.

**OS layer.** The firmware wraps FreeRTOS (all in `fw.h`, all for CPU0 only):
- tasks: `fw_task_create(entry, arg, name, priority, stack)` (0xCD88A4) and `fw_task_join`
  (0xCD8960), which waits for `entry` to return;
- binary semaphores: create, take (-1 on timeout), give, delete (0xCD86DE, 0xCD874C, 0xCD876E,
  0xCD86E8);
- firmware heap: `fw_os_calloc` and `fw_os_free` (0xCD8610, 0xCD8654);
- `fw_os_sleep(ms)` (0xCD86B4).

On failure, the allocation and task functions return `printf`'s return value instead of NULL.
`fw_is_ram()` checks results.

**SDK sound module** (`sdk/src/sound.c`) follows the video player:
- a sound handle is a worker; its memory holds the state;
- the worker's task, at priority 6, owns the device. It returns at once if the device is busy.
  Otherwise it opens it at 44.1 kHz stereo, 8 periods of 256 frames, with the video player's PA
  callback, then renders one period at a time with blocking writes;
- `fw_sound_play` hands requests to the task through a small lock-free queue in that memory, and
  drops them until the worker runs;
- `fw_sound_close` releases the worker. The task fills the ring with silence, waits for it to run
  dry (`FW_AUDIO_HW` state, polled with `fw_os_sleep`), closes the device and returns; the
  service then joins it and frees the memory.

It mixes up to 4 synthesized voices: square, triangle or noise, with a linear frequency slide and
a decaying envelope. The games have no sound while the music player has the audio device open.
Whether the music player keeps the device open while paused or stopped hasn't been tested yet.

### Album art

`sdk/apps/albumart` shows the playing MP3's embedded cover as the background of the "now
playing" page (page 4). It has no menu row; its `patches.json` hooks two places:
- **The playing file.** MediaTask starts every track with `fw_media_start(path, type)`
  (0xD21800, 16 calls in the `_media_*_process` state handlers, type 3 for music). The hook
  copies the path into a shared record (module slot `FW_MODULE_ALBUM_ART`) with a sequence count,
  then calls the original. This runs on CPU0, in MediaTask, and only copies memory.
- **The page.** The `bl page_music_play_create` (0xD12E64) at 0xD04ABA in `view_page_create`
  goes to a hook that builds the page, then starts a worker. The page state at
  `*(FW_VIEW_CTX) + 0xA0` holds its labels; their parent is the page's container, a full-screen
  opaque object and the screen's only child. The cover is an `lv_img` that the hook moves to the
  front of the container's children (`spec_attr` at +8 holds the child array and count; the
  firmware has no `lv_obj_move_background`), so LVGL draws it before the labels. The page's
  `LV_EVENT_DELETE` releases the worker.
- Setting the cover as the container's background image (`lv_obj_set_style_bg_img_src`,
  0xD2E122, property 0x26 in this LVGL) drew it over the labels instead; the cause wasn't found.
  The background image ids are 0x26-0x2A here, as `lv_obj_init_draw_rect_dsc` (0xD2ABA8) reads
  them, not LVGL 8.3's.

The worker (about 47 KB on the firmware heap) waits for a new path, then:
- **Finds the cover.** It scans the ID3v2 tag (versions 2.2 to 2.4, with unsynchronisation) for
  APIC (or PIC) frames holding a JPEG, preferring the front cover (type 3). PNG pictures and
  compressed or encrypted frames are skipped.
- **Decodes it** with the firmware's own TJpgDec, the photo viewer's decoder (`fw_jd_prepare`
  0xCF1A20, `fw_jd_decomp` 0xCF1F90; see `fw.h` for its state layout). It picks the largest
  power-of-two reduction that still covers the screen. A progressive JPEG only decodes from its
  first scan, at 1/8 detail. The firmware also has libjpeg, which the video player uses for
  MJPEG; TJpgDec is simpler to drive and scales while decoding.
  - This TJpgDec is extended for the photo viewer: in an APP1 (Exif) segment it looks for a
    thumbnail, asking the input callback for its position and seeking back (flags at 0xA8 and
    0xAC of its state). Its sixth argument 1 skips APP1 segments, which album art uses; with 0,
    an Exif thumbnail of 96x120 or more is decoded instead of the picture.
- **Draws it.** The output callback crops the picture around its center to fill 128x160, halves
  its brightness so the labels stay readable, and swaps the RGB565 bytes for LVGL.
- **Swaps covers safely.** Before overwriting a cover on screen, it has the UI hide it.

### Recording

**`/dev/audio1`** is registered with the same probe as `/dev/audio0` (0xD46300, descriptors at
0xCB4E90). Its driver functions are open 0xD46124, close 0xD46108 and read 0xD46068, which jumps
to the stream read at 0x813A74 in RAM. `fw_dev_read(node, buf, frames, 0)` (0xD4469C) calls it.

Its 10 open parameters have the `/dev/audio0` layout, except word 7:
- channels, bytes per sample, sample rate, period (frames), periods, start threshold, 0, 0;
- word 7: byte 0 is the AMIC gain (only used with 2 channels; 0 sets the defaults 0x1C and
  0x54), byte 1 the input source. Each source switches on a different set of codec inputs
  (`FUN_00D4606E`);
- event callback and argument, which open does not use.

A read takes any number of frames, starts capture if needed, and blocks (polling every 1 ms, for
up to 3 s) until they are in. Close stops capture and frees the stream.

The firmware's sources:
- A debug shell command (`FUN_00CDA798`) records 2 channels at 16 kHz, period 128, 2 periods,
  source 7 into a buffer, then plays it back on `/dev/audio0`.
- The voice recorder's PCM input (`FUN_00D49500`, "recopcm") picks the channel count from the
  source: 1 for sources 0-4, 9 and 10, 2 for 5-8 and 11, 3 for 12 and 13. Which source the
  recorder uses hasn't been traced yet.

**SDK mic module** (`sdk/src/mic.c`) uses the debug shell's settings. With them, the player's
microphone is on the right channel; the left one stays silent. A worker task owns the device and
reads one period at a time into a ring of 1024 frames in the worker's memory; `fw_mic_latest`
copies the newest frames out, `fw_mic_failed` tells when the device couldn't be opened, and
`fw_mic_close` releases the worker, like the sound module.

### SD card

**`/dev/sd`** (probe 0xD46D58, descriptor at 0xCB5038) has driver functions read 0x813390 and
write 0x813362 (both in RAM), ioctl 0xD46EB4, open 0xD46DA4 and close 0xD46D90.

FatFs's disk functions (0xD59E64-0xD59F4E) use it:
- `sdmmc_wrap_init` (0xD59CDC) opens `/dev/sd` for a drive and mounts it. A table of 13-byte
  entries at 0x81A7CC holds each drive's node, `FATFS` object, mounted flag and device name. All
  disk calls use drive 0's node, `FW_SD_NODE`.
- `disk_read` is `fw_dev_read(node, buf, count, &sector)` and `disk_write` the same with
  `fw_dev_write`: the last argument points to the first sector's number (32 bits). Both return
  `count` on success and 0 on error.
- The read goes through `FUN_00816538(card, buf, sector, count)`. It holds the card's mutex,
  reads one 512-byte sector at a time, and uses a bounce buffer for unaligned `buf`.

The device ioctl doesn't return the card's size: command 1 calls the media ioctl but drops its
result. The media ioctl itself, `fw_sd_ioctl(cmd, &out)` (0x816778), answers FatFs's commands:
1 sector count, 2 sector size (512), 3 erase block size (1).

**Don't read the card from UI code either.** An early SD map read 16 sectors per 20 ms UI
timer tick; it started drawing sectors, then rebooted the player after a couple of seconds.

**SD map and Flash map** (`sdk/apps/blockmap`, two APP menu rows) read from a worker task
(priority 4, pausing 2 ms after each row of 16 blocks), which also asks the card for its size,
and store a color per block in the worker's memory; a UI timer draws them.
SD map reads 512-byte sectors; Flash map reads the memory-mapped flash (`FW_FLASH_BASE`) in
4 KB erase sectors, two pages for the 2 MB chip. Each block's color is the Shannon entropy of
its bytes, computed with an integer log2, from blue (0 bits per byte) to red (8). Random data
comes out at about 7.6 in a 512-byte sector.

### Files

The firmware's FatFs is R0.14b-style. `fw.h` binds its API, with the structures as opaque
buffers of the right size (`fw_file_t` 600 bytes, `fw_dir_t` 88, and `fw_fileinfo_t` 288 with
its fields).

| Function | Address | Function | Address |
|---|---|---|---|
| `f_open` | 0xD58890 | `f_opendir` | 0xD58086 |
| `f_close` | 0xD58D60 | `f_readdir` | 0xD58190 |
| `f_read` | 0xD57918 | `f_closedir` | 0xD58162 |
| `f_write` | 0xD57AD6 | `f_stat` | 0xD5825E |
| `f_lseek` (64-bit offset) | 0xD57CCE | `f_getfree` | 0xD582CC |
| `f_sync` | 0xD58BB4 | `f_unlink`, `f_rename`, `f_mkdir` | 0xD583EA, 0xD584F4, 0xD58D92 |

Also present: `f_mount` 0xD57888 (only `sdmmc_wrap_init`/`deinit` call it), `f_findfirst`
0xD58240, `f_findnext` 0xD58210 and `f_mkfs` 0xD58F38. Not linked: `f_chdir`, `f_gets`,
`f_printf`, `f_truncate`, `f_utime`. There is no file layer above FatFs worth using.

**Configuration.**
- Long names are allowed, with a 0x460-byte working buffer from the firmware heap per path call
  (`FF_USE_LFN` 3).
- Strings are UTF-8 (`FF_LFN_UNICODE` 2). 8.3 names use code page 932.
- exFAT, fast seek and `f_findfirst` are enabled.
- Volumes `"0:"` and `"1:"` are the first two partitions of the card, and the drive prefix is
  optional. `/` and `\` both separate folders.
- There is no current directory.
- Up to 20 files and folders can be open at once (`FF_FS_LOCK`), shared with the firmware.
- `FILINFO` holds a 64-bit size, date, time, attributes, an 8.3 name and a 256-byte name.
- Result codes and mode flags are standard FatFs.

**Locking.** Each volume has a FreeRTOS mutex (`FF_FS_REENTRANT`), so calls are safe from any
task on CPU0, and never from UI code (see [Two cores](#two-cores)).

**Mounting.**
- `watch_sdcard_init` (0xD26CB8) mounts the card at startup, on insertion and on wake.
- `watch_sdcard_deinit` unmounts it on removal, in USB card-reader mode and on sleep. It frees
  the `FATFS` without waiting for other users of the card.
- Calls on an unmounted card return `FR_NOT_ENABLED` (12).
- If mounting fails, the firmware frees the `FATFS` but leaves its pointer behind. So before
  touching the card, check `fw_sd_mounted()`: the drive table's mounted flag (0x81A7D4) and
  `FatFs[0]` (0x81DC80) must both be set.
- Keep files open only briefly.

**Heap.** Each path call and `f_readdir` takes 0x460 bytes through the firmware's `oal_malloc`.
When that fails, it returns `printf`'s result instead of NULL, and FatFs then writes through the
bad pointer, which crashes the player. So make sure the firmware heap has room before calling.

**Files** (`sdk/apps/files`) browses the card and shows files in hex or as text.
- A worker task makes all the FatFs calls. The UI posts a request (list a folder, or read 512
  bytes at an offset) in the worker's memory and polls for the result.
- Each read opens, seeks, reads and closes the file, so nothing stays open.
- The task skips a request ("Low memory") when the firmware heap has less than 8 KB free.
- Folders are listed into a 12 KB name pool (up to 400 entries) and sorted with `rom_qsort`,
  folders first.

### Diagnostic apps

**Task monitor** (`sdk/apps/taskmon`) lists the FreeRTOS tasks once a second. A worker task
takes the snapshot with `rom_task_states` (the ROM's `uxTaskGetSystemState`) and publishes it
with a sequence count. The footer also shows the firmware heap and LVGL's UI heap
(`lv_mem_monitor`, 0xD3904C: free, largest free block, lowest free while open).
- **Task data:** each `fw_task_status_t` holds the name, number, state, current and base
  priority, run time and lowest free stack (in words). The kernel has 20 priority levels.
- **CPU use:** run-time stats are on, counted by the low 32 bits of the hardware timer at
  0x40082104. A task's CPU use is its run time since the last snapshot over the total. "Busy" is
  100% minus the `IDLE` task's share. Run time is only added when a task is switched out, so a
  task that never yields shows 0%.
- **Heap:** heap_4 keeps its size, free bytes and minimum free bytes at 0x800C74, 0x800C64 and
  0x800C68 (`FW_HEAP_SIZE`, `FW_HEAP_FREE`, `FW_HEAP_MIN_FREE`). `rom_heap_init` (0x9D28) sets
  them up.

**Benchmark** (`sdk/apps/bench`) times, on the ROM's hardware timer from a low-priority worker
task on CPU0:
- the CPU clock, from the DWT cycle counter, or from the SysTick reload at 1 kHz if the counter
  doesn't run;
- `memcpy`, a byte loop and `memset` over 8 KB buffers;
- `memcpy` from memory-mapped flash (512 KB of FIRM);
- float fused multiply-adds, software `double`, `rom_sinf`, and 64-bit division.

**Pin monitor** (`sdk/apps/pinmon`) polls all 128 GPIO pins 50 times a second with
`rom_gpio_get`, and highlights pins as they change.
- The ROM's pin table at 0x9B4 puts the 4 ports at 0x40081000 + 0x40 × port, with the input
  levels at +0x10.
- A pin is `port << 16 | pin << 11`; the low bits configure it in `rom_pin_config`.
- Of the buttons, only Center (the power key) changes a GPIO pin. The others don't show up, so
  they must be read another way, most likely through the ADC (`/dev/kadc_ch1`).
- `rom_gpio_get` is a plain register read, safe from UI code; the image is in a memory-only
  worker.

**Crash log** (`sdk/apps/crashlog`) shows the black box's state (see [Two cores](#two-cores)),
turns `0:/bbstatus.txt` on and off (Up), and has two tests: holding Center makes the UI core
fault, holding Up makes it hang. Both should leave a `0:/crashlog.txt` before the reset.

### Buttons and key events

The keypad driver's read callback (`FUN_00D02018`) turns the key manager's (key id, event)
pairs into the codes below. Short presses and the Left, Right and Up long presses were measured
with the Key test app; the rest come from the driver code.
- **Holding a key** sends the long-start code once, then the repeat code a few times per second,
  then the long-release code when the key is let go.
- **`fw.h` names:** `FW_KEY_<KEY>` for the short press, plus `_LONG`, `_REPEAT` and `_LONG_UP`.

| Button | Key id | Short | Long start | Repeat | Long release | `fw.h` name |
|---|---|---|---|---|---|---|
| Left | 0x42 | 0x14 (`LV_KEY_LEFT`) | 0x85 | 0x87 | 0x86 | `FW_KEY_LEFT` |
| Right | 0x43 | 0x13 (`LV_KEY_RIGHT`) | 0x88 | 0x8A | 0x89 | `FW_KEY_RIGHT` |
| Top | 0x25 | 0xA0 | 0xA1 | 0xA3 | 0xA2 | `FW_KEY_UP` |
| Center | 0x21 | 0x0A (`LV_KEY_ENTER`) | 0x8B | (0x8D) | (0x8C) | `FW_KEY_CENTER` |
| Bottom | 0x45 | 0x81 (the firmware's "leave page") | 0x82 | 0x84 | 0x83 | `FW_KEY_BACK` |

The driver also maps keys this player doesn't have:
- 0x20: `LV_KEY_DOWN`, long 0x99/0x9B/0x9A;
- 0x22: `LV_KEY_UP`, long 0x96/0x98/0x97;
- 0x34 "record": 0x92, long 0x93/0x95/0x94.

The key manager (`FUN_00CDD5C0`, a state machine logging "key status is work" and so on) names
some other ids: 0x23/0x24 volume down/up, 0x37 "onoff" and 0x47 "power". Its state byte is at
0x81BC50 (1 = screen on), and it hands keys to the keypad driver with `FUN_00D0227C(id, event)`.
The driver holds a single pending pair (at 0x823DF4).

**Back long press.** With the screen on, the key manager opens the volume control on a Back long
start (`FUN_00CDD5C0`, branch at 0xCDDD08) instead of passing it on. While the volume control is
open, Left/Right adjust the volume. Back's repeat and long release reach the keypad driver
normally.

**Center is different.** Its button goes through the power management chip, which reports only
three raw events to the key manager: press (1), short release (2), and long hold (4). The key
manager turns 4 into a press of the "power" key (0x47), which powers the player off when the
status byte read by `FUN_00D27A5C` is 0 or 1, and turns the screen off for 2 to 5. That byte is
firmware state (at 0x81D14C) with its own dispatcher (`FUN_00D27158`), probably Bluetooth, so the
SDK leaves it alone. So Center never produces repeat or long-release codes, and holding it powers
the player off.

The SDK patches the key manager's "keys ready" check (`bl 0xCF09D4` at 0xCDD5E6, entry in
`sdk/patches.json`) to `hook_key_event` (`sdk/src/keys.c`):
- If the focused object has `FW_OBJ_FLAG_CENTER_LONG` (`LV_OBJ_FLAG_USER_1`), the hook rewrites
  raw event 4 as a Center long start (0x21, 0x40), which arrives as `FW_KEY_CENTER_LONG` (0x8B).
  Confirmed on the device; no repeat or release code follows.
- If the focused object has `FW_OBJ_FLAG_BACK_LONG` (`LV_OBJ_FLAG_USER_2`) and the screen is on,
  the hook passes a Back long start straight to the keypad driver, so the app gets
  `FW_KEY_BACK_LONG` (0x82). It then replaces the event with raw 0, an unknown key that the key
  manager ignores, so the volume control doesn't open.
- SDK overlays set both flags on their root. Clear a flag on `ov->root` to restore the firmware's
  action.
- Not yet confirmed: whether the power chip forces the player off after an even longer Center
  hold.

The games exit on `FW_KEY_BACK_LONG` and ignore a short Back, so a stray press doesn't end a
game. In Mp3tris, a Center press drops the piece one row, and a press on a landed piece locks
it. Center sends no repeat codes, so holding it (`FW_KEY_CENTER_LONG`, sent once) drops the piece
to the bottom and locks it.

Mandelbrot and Pin monitor exit on a held Back and use a short Back as "down", since Back is
the bottom button and the player has no Down key. Center zooms in on its release (`FW_KEY_CENTER`), and
holding it zooms out (`FW_KEY_CENTER_LONG`): a long press sends no release code, so the two
never both fire.

Center (ENTER) works differently from the others:
- `LV_EVENT_PRESSED` arrives when it goes down.
- `LV_EVENT_RELEASED`, and then its `LV_EVENT_KEY`, arrive when it is let go.

For an immediate reaction, act on `PRESSED`. Use the `KEY` event only as a fallback when no
`PRESSED` came first; Flutter and Mp3tris both do this. Event codes in this build: PRESSED 1,
RELEASED 8, KEY 13, DELETE 33.

Page handlers also check 9/11 (NEXT/PREV), 27 (ESC), and the repeat codes 0x87, 0x8A, 0x98, 0x9B
and 0xA3 and record code 0x92 from the table above.

### Ghidra

- `ghidra/import.sh [firmware.bin [bootrom.bin]]` builds `ghidra/project/sl6801.gpr` with two
  programs, both with the boot ROM at 0 (bx lr stubs if there is no `bootrom.bin`):
  - `sl6801_app`: the main firmware, about 4,700 functions (370 named), plus 1,500 in the ROM
    (100 named).
  - `sl6801_boot`: the bootloader. Only its block at 0x81FBC0+ is the bootloader proper.
- The scripts in `ghidra/scripts/`:
  - `SL6801Setup.java` sets up the memory map (flash, RAM copies, the ROM), names the ROM
    functions from its `ROM_FUNCTIONS` table, and seeds functions from vectors and code pointers.
  - `SL6801Names.java` names functions after their `__func__` strings and exports
    `ghidra/out/<program>_functions.csv`.
  - `SL6801Fixups.java` clears the "no return" flag that analysis puts on returning ROM
    functions such as `rom_printf` (0x2B0).
- `ghidra/decompile.sh <app|boot> <out.c> <name|0xaddr>...` decompiles from the saved project
  without re-analysis.

### Windows flashing setup

- **Driver.** In bootloader mode the device (`301A:2800`) binds to Windows' USB mass-storage
  driver. In Zadig, use Options > List All Devices and replace that driver with WinUSB for this
  VID/PID only. Card reader mode (`301A:2801`) is unaffected.
- **Flasher.** [smartlink_flash](github.com/ilyakurdyukov/smartlink_flash).
- **Building it.** In MSYS2 UCRT64, run `pacman -S mingw-w64-ucrt-x86_64-libusb make`, then
  `make CC=gcc` from a UCRT64 shell. Copy `libusb-1.0.dll` next to `smtlink_dump.exe` so it runs
  from any shell.
- **Using it.** Always start a command with `init`, e.g.
  `smartlink_flash/smtlink_dump.exe init flash_id read_flash 0 2M backup.bin`.

## Gotchas

- **App UI code runs on CPU1, without the kernel.** Kernel, FatFs and device calls from it freeze
  the player at random; use workers (see [Two cores](#two-cores)).
- **No writable globals in apps** (see [Writing a new app](#writing-a-new-app)).
- **Center's KEY event comes on release**; use `LV_EVENT_PRESSED` for responsiveness.
- **`LV_EVENT_DELETE` is 33** in this build (not 34).
- **`fw_task_create`'s stack size is in words** (it goes straight to `xTaskCreate`), taken from
  the firmware heap. Worker stacks are given the same way; the apps use 0x400 (4 KB).
- **Keep the firmware heap from running out.** Its `oal_malloc` returns `printf`'s result
  instead of NULL on failure, and callers such as FatFs then write through that bad pointer.
- **Images resized by `pack`** may be cropped or misplaced if the UI code hardcodes the original size.
- **`slfw.Image` copies its input buffer**; code that edits an image must write to `img.data`.

## Open questions

- What the 130 KB zero run inside FIRM is for, and FIRM header field +0x10.
- Whether the vendor firmware ever writes to the free flash area (no problems seen so far).
- What CPU1's entry at 0x80DA4F does before it runs the UI loop, and how the firmware passes the
  UI loop to it (`FUN_00CD84BC`).
- Most of the ROM's audio and Bluetooth functions are still unnamed.
- Somewhere persistent to store app data (e.g. high scores) without globals: a free field in the
  `PSMP` settings, a spare pointer in the firmware's RAM, or a file on the SD card (`f_write` is
  bound but not tried yet).

## License

MIT
