# Adafruit RTD266x Firmware

Experimental RTD2660H display-controller firmware built with SDCC. The first
target is a UCTRONICS UC-586 with a generic 5-inch RGB888 800x480 panel.
No Keil compiler, vendor library, stock firmware image or inherited source tree
is required to build it.

This is a new implementation informed by register documentation, firmware
analysis and earlier bench experiments with
[ORTD2662](https://github.com/KerJoe/ORTD2662) and
[tkdesign-jp's port](https://github.com/tkdesign-jp/ORTD2662). It is not a formal
clean-room implementation. Hardware facts and unresolved assumptions are
recorded in [video notes](docs/video-registers.md) and
[display-control notes](docs/ui-capabilities.md). New source and category icons
are MIT licensed. The bundled Roboto Mono font and its generated bitmap
glyphs are under SIL OFL 1.1; see the [font source and license](assets/fonts/README.md).

## Build

Use GNU Make and SDCC on Linux, macOS or Windows through WSL. On Ubuntu:

```sh
sudo apt install sdcc make gcc python3 python3-pil
make
make check
```

The default build is equivalent to:

```sh
make BOARD=uc586 PANEL=rgb800x480 APP=monitor SPLASH=1
```

It produces `build/uc586-rgb800x480-monitor-splash1/firmware.bin`, a 65536-byte
bank0 image. Local development uses SDCC 4.5.0. Firmware variants get separate
output directories so changing a board, panel, application or splash setting
cannot reuse the other variant's object files.

The default build shows a full-screen black startup background with a centered
white Adafruit flower and wordmark bitmap. The splash stays visible for one
second after loading, then the firmware acquires and displays input video.
The panel free-runs during startup, so the screen does not require an input
signal. The bitmap is split into tiles
stored in the scaler's OSD SRAM. Replace `assets/splash.bmp` and run `make` to
customize it; the build generates the bitmap header automatically using Pillow.
BMPs can contain up to 15 visible colors plus transparent black; richer images
are quantized automatically. Use `make SPLASH_BMP=path/to/my-logo.bmp` to select
another file. Larger BMPs, including full-size 16-bit RGB565 images, automatically
shrink to fit 192x108 pixels without cropping and are displayed at 4x scale.
RGB565 input is converted to the OSD palette; this is not a full-color framebuffer.
See the [custom splash guide](assets/README.md) for the complete BMP-to-firmware
workflow, example commands, timing and image limits. Use
`make SPLASH=0` to default it off; saved menu preferences take precedence.
That build uses a separate `-splash0` directory.

When input is absent, a separate Adafruit TV test card displays "NO SIGNAL" on
the black background. It disappears when valid video returns. Customize it with
`make NO_SIGNAL_BMP=path/to/my-no-signal.bmp check`; this works even with
`SPLASH=0` and uses the same BMP dimensions, palette conversion and centering.

On acquisition, a top-left overlay shows the input resolution, estimated refresh
rate, horizontal frequency, sync polarity and measured totals for three seconds.
Unsupported input keeps its measured settings and the first rejection reason on
screen. Missing measurements display `--`; disconnected input retains the TV
test card. These messages do not expand the supported video modes.

Menus and timing messages use a true monospaced Roboto Mono font in native
12×18 antialiased cells, with lowercase, uppercase, digits and punctuation.
The glyphs share one pen origin and baseline, preserving their designed side
bearings. Their 2-bpp coverage maps to four palette colors. The live menu uses
native 1× size at 360×216 pixels, centered by default; timing messages retain
2× zoom. The font uses lossless compression in firmware and expands directly
into OSD SRAM with identical glyph pixels. Generated font and icon headers are checked in,
so ordinary firmware builds do not need a font converter. To regenerate them
and inspect a glyph sheet, run `python tools/font_to_header.py --preview
build/menu-font.png` with the versions listed in the
[font regeneration guide](assets/fonts/README.md).

`make TRACE=1` enables bench diagnostics in the EDID ASCII descriptor and MCU
scratch registers. Its output directory ends in `-trace`. This changes the
descriptor during measurement; use the default `TRACE=0` for ordinary display
operation. See [diagnostic decoding](docs/video-registers.md#bench-diagnostics).

## Design

| Directory | Responsibility |
| --- | --- |
| `boards/` | Pin routing, buttons, backlight and other board wiring |
| `panels/` | Physical pixel and line timings |
| `src/platform/` | 8051 startup, timer, scaler gateway and EDID SRAM access |
| `src/rtd/` | Video and audio acquisition, clocks, scaling, EDID and OSD |
| `src/app/` | Input policy, live menus, shared settings and soft power |
| `include/rtd/` | Small interfaces between those layers |
| `assets/` | Editable startup and no-signal BMPs, with artwork attribution |
| `tools/` | Build-time BMP conversion and Feather tester/programmer |
| `tests/` | Host checks for timing arithmetic, register encoding and rejection paths |

The video driver names the scaler page on every register access. The interrupt
handler only maintains time; it never competes for the shared scaler gateway.
Panel profiles use physical units, leaving register encoding inside the driver.
There is no dynamic allocation or dependency on a proprietary runtime.

Only the supplied UC-586/800x480 combination is implemented. Video acceptance
supports native 800x480 at 1000x525 timing, PicoDVI's alternate 800x480 at
992x500 timing, and centered 640x480 at 800x525 timing. Keep aspect is the
default: 640x480 stays at 1:1 with 80-pixel black sidebars; 800x480 fills the
panel. The live Display menu can select `Keep` or `Fill`; fill expands 640x480
across the panel. All supported inputs are
near 60 Hz. These are explicit timing profiles, not arbitrary mode detection;
see the [timing contract](docs/video-registers.md#supported-timing-contract).
Capture uses the measured sync polarity, with two matching mode/polarity
samples required before displaying a newly acquired source.

## Display controls

The live menu has a left column of Picture, Audio, Display and Menu Settings
icons, with the selected category's current settings shown alongside. Menu
enters the settings pane; Back returns to the same category. Mixed-case text,
outlined icons, a selection bar and percentage sliders use the antialiased
font palette. Menu Settings scrolls to reach No Signal, OSD Setup and System
submenus. Menus are currently English only.
The RP2350 tester sends menu/select, back, up, down and power
events over DDC/CI while video runs. Physical button decoding remains pending.
See the [control map and host commands](docs/ddcci.md).

Image brightness and contrast adjust video pixels. LED backlight adjustment
is disabled: PWM1 requests at 100%, 25% and 0% produced no visible brightness
change in three camera captures. Picture includes RGB gains, saturation and
horizontal sharpness, with 50% neutral for each. The live menu also controls
volume, mute, aspect, startup splash, connection popups and menu timeout.
Aspect offers Keep (default), Fill, forced 4:3 and forced 16:9. The forced
ratios compress the complete picture without cropping: 4:3 uses a centered
640x480 area, and 16:9 uses 800x450 with black bars above and below. Both passed
640x480 and native 800x480 bench checks with 525-line source timings. The
500-line CVT timing falls back to Keep for 16:9; its 4:3 path has host-test
coverage only. `ASPECT_4_3=0` or `ASPECT_16_9=0` excludes a forced ratio from
the build. The [register notes](docs/video-registers.md) describe these limits.
Additional software controls are:

- OSD Setup: horizontal/vertical position across the visible panel, with 50%
  centered; horizontal movement uses four-pixel steps. Transparency changes
  menu backgrounds across eight hardware levels. Its 0–100% slider spans
  opaque through 7/8 video blending, while text remains opaque. Other overlays
  retain their own position and opacity.
- No Signal: black, blue or test-pattern background; backlight sleep after
  1, 2, 5, 10, 20, 30, 40, 50 or 60 seconds, or Never.
- System: an independent 1–120-minute sleep timer (0 is Off), factory reset
  with a confirmation page, and a temporary burn-in color test. Burn-in cycles
  red, green, blue, white and black every two seconds; the menu and DDC remain
  usable to stop it. Power off or restart cancels the test.

The added menu pages, position/transparency, reset, sleep timer and burn-in
passed UC-586 camera and DDC checks. Expanded preferences restored after a
whole-chip reset. Horizontal sharpness remains responsive during its background
filter upload and showed a modest edge change; this is not a calibrated image
quality measurement. See the [per-control bench record](docs/ddcci.md#transport-and-validation)
for qualification and remaining limits.
Changes save
to the UC-586's EEPROM after two seconds and restore at startup, including
the splash preference before anything is drawn. `make SETTINGS=0` disables
saving and uses a separate `-volatile` build directory. No-signal sleep requests
backlight power off and on when valid input returns. Its separate stock-derived
P6.4 gate passed physical backlight-off and wake checks on the UC-586.

Shipped RTD2660H boards demonstrate whole-screen 180-degree
rotation and mirroring, but the mechanism and available modes are board
dependent. See the [capability and firmware-analysis notes](docs/ui-capabilities.md).
The first audio profile implements stereo 48 kHz LPCM through the UC-586's
CS4334 DAC. See [audio support and validation](docs/audio.md) for its current
bench status and limits. LED backlight, mirror and rotation are disabled
in the menu; LED backlight shows a gray `--`.
EEPROM saving and restoration after whole-chip reset are bench-tested. The
firmware preserves stock data and refuses an occupied, unrecognized save area.
See [settings storage](docs/ddcci.md#settings-storage) for the reservation,
backup procedure and power-loss behavior.
Arbitrary video modes are not implemented. The earlier six-page menu, navigation,
editing and setting readback passed the [DDC/CI bench checks](docs/ddcci.md#transport-and-validation),
including uninterrupted audio during menu drawing. Camera and audio checks also
confirmed picture adjustments, Keep/Fill aspect, mute, soft power and no-signal
backlight sleep/wake. Adjustable backlight dimming remains unavailable.

Build with `make MENU_PREVIEW=1` to cycle through the main menu and all four
submenus after the splash. Each page shows three sample selections/values,
including empty, half-full and full sliders. This opt-in artwork preview has
no button actions and changes no settings. It is a renderer exercise separate
from the live menu; normal builds proceed directly to video acquisition and
open the live menu only when requested. The static preview renderer and its
sample strings are excluded from normal firmware to preserve code space;
host OSD tests explicitly enable them.

## Programming

Use the included [Feather tester/programmer](tools/tester/README.md).
Its RP2350 HSTX variant supports both HDMI audio testing and programming through
the same connected HDMI cable. Select `mode off` before programming and return
to `mode 640` afterward; mode changes reboot the Feather.
Save matching complete reads of your own board's original flash and preserve
its protection state before programming. The tested UC-586 has a W25X40
(`EF3013`) with 512 KiB of flash. Other boards can have different flash and pins.

The programmer expects a full image. Build one from the new bank0 and the
untouched tail of your own verified backup:

```python
from pathlib import Path

bank0 = Path("build/uc586-rgb800x480-monitor-splash1/firmware.bin").read_bytes()
original = Path("original.bin").read_bytes()
assert len(bank0) == 65536 and len(original) == 524288
with open("firmware-full.bin", "xb") as output:
    output.write(bank0 + original[65536:])
```

From `tools/tester/feather_rp2040/Feather_DVI_RTD_Tester`, use the shared
`host.py` for either Feather:

```sh
python host.py restore-protection /path/to/current-verified.bin \
  --status 0x1c --allow-write --receipt protection.json
python host.py program /path/to/firmware-full.bin \
  --backup /path/to/current-verified.bin --allow-write --receipt program.json
python host.py reset-chip
```

For the supported UC-586/W25X40, enable whole-flash protection (`0x1C`) before
installing fresh firmware. The first command verifies every byte against the
current backup before changing protection. Settings use the separate EEPROM.
Partial protection (`0x0C`) allowed intermittent retained-vendor-byte changes
during ISP release or early execution on this board; the exact writer remains
unresolved. Keep full protection enabled between programming sessions.

The backup must match the image currently installed, which is the original
only on the first run. The programmer compares the complete current image,
writes changed sectors, verifies all bytes and restores flash protection.
The final `reset-chip` command is required after programming: an ISP-only MCU
restart retained DDC peripheral state on the bench, and a whole-chip reset
restored the live interface. Run it before returning the tester to video mode.
Keep unexpected readbacks before making further changes. The retained flash
tail is not linked into the new program and is not a settings-storage area.
Once CRC-capable firmware is installed, [live CRC and `program --fast`](tools/tester/README.md#firmware-crc-and-faster-bank0-updates)
can verify the 64 KiB application bank without a final full-flash readback.

## Validation status

On 2026-09-29, the fresh implementation displayed native 800x480 text and grids,
aligned the alternate 992x500-total 800x480 grid, expanded 640x480 text and grids
to the full panel, displayed the no-signal test card on signal loss, and
reacquired native video when the source returned. The full-screen startup
background and centered bitmap were verified after a whole-chip reset with
the source enabled and disabled, followed by handover to video. These checks
used the Feather DVI source, not a general
HDMI compatibility suite. Physical cold boot was tested on the initial video
implementation; the bitmap splash was tested by whole-chip reset. Bitmap upload
precedes the one-second hold, so the black background appears before the logo.
The color renderer was checked on the UC-586 with a 15-color chart, transparent
gaps and return to video. Packing tiles during conversion makes the chart
visible in the first startup capture, about two seconds after reset.

SDCC 4.5.0 and host checks pass for splash-on, splash-off and diagnostic builds,
including a binary/map check that all six interrupt vectors reach the linked
handlers and a pixel-by-pixel reconstruction of the bitmap from OSD writes.
BMP tests cover color preservation, palette reduction, transparent black,
row/tile/plane ordering, size limits and unchanged generated output.
The centered 640x480 default passes host register checks and the SDCC build;
on 2026-09-29, a UC-586 camera capture confirmed the centered grid and black
sidebars. Programming verified all 512 KiB and restored the original protection byte
`0x0C`. Builds, code and register notes are provided; stock firmware dumps and
the preserved original flash tail are not distributed.

The RP2350 HSTX audio test confirmed a clean 1 kHz analog tone from the UC-586
headphone jack, muting on source loss and recovery with video. Splash, no-signal
artwork and the scaled grid remained visible. The input timing overlay was
subsequently restored by preserving its enable across hardware background
transitions; camera captures confirmed the timing text and its later expiry.
The opt-in menu preview's text and colored selection bar were also verified.
See the
[audio measurements and limits](docs/audio.md#validation).
