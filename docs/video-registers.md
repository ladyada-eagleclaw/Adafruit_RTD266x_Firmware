# Video implementation evidence

This driver is a documented reimplementation. Its structure, validation,
arithmetic, and generated linear filter are new. Register addresses and hardware
behavior were learned from the RTD2660 register manual and earlier UC-586 bench
experiments using the [ORTD2662 project](https://github.com/tkdesign-jp/ORTD2662).
No vendor source, stock firmware, or inherited filter table is included here.
The earlier firmware's results informed this implementation; its own bench
results are listed in the README.

## Supported timing contract

The panel profile describes physical units: 800x480, 1000 clocks per line,
525 lines per frame, nominal clock 31.5 MHz, negative HS/VS, positive DE,
HS width 48, VS width 3, active start (88, 32). The hardware encodes total
horizontal clocks minus four and horizontal output edges minus ten (manual
pp31–34). The fixed-last-line registers receive the physical 1000/525 totals.

Input is deliberately limited to three timing profiles near 60 Hz:

| Input | Input total | H/V sync | Capture start registers | Output V total/start | Frame delay register codes |
| --- | --- | --- | --- | --- | --- |
| 800x480 native | 1000x525 | -/- | H=86, V=32 | 525/32 | CR40=2, CR41=40 |
| 640x480 VGA | 800x525 | -/- | H=142, V=35 | 525/32 | CR40=5, CR41=44 |
| 800x480 PicoDVI CVT | 992x500 | -/+ | H=166, V=17 | 500/10 | CR40=9, CR41=40 |

The alternate timing comes from
[Adafruit PicoDVI's timing definition](https://github.com/adafruit/PicoDVI/blob/master/src/libdvi/dvi_timing.c):
H front/sync/back 24/72/96 pixels, V front/sync/back 3/10/7 lines, 29.52 MHz
nominal pixel clock. Its sync-relative capture starts at H sync+back-2 and
V sync+back. CR11 bit2 inverts H and bit3 inverts V (manual p22), so CVT uses
`0x04` in bits3:2;
the other two profiles use `0x0C`.

Output horizontal total stays at 1000, with its line rate matched to input.
CVT needs shorter output vertical blanking: start 10/end 490 fits within 500
lines, whereas the default start 32/end 512 would overrun this input frame.
Both timing-port totals and page 1 fixed-last-line totals follow the profile.
Manual p34 says the programmed display V total is a watchdog reference in
frame-sync mode; actual frames follow input VS. The nine-line frame delay
keeps roughly the same capture-to-display buffering as the native profile.
This short-blanking raster is specific to the tested UC-586; it is not a claim
that every RGB panel supports a 500-line frame. The generic KD50G21 reference
lists a 513-line minimum for sync mode, separately from its DE-mode timings.

The capture offsets and frame delay codes produced aligned test grids for all
three profiles on the UC-586 on 2026-09-29. They are empirical calibration, not
a general mode solver.
The manual p42 describes CR41 as `16*code + 16` clocks for nonzero codes; older
bench notes called these `16*code`. The driver preserves the measured register
codes and makes no stronger claim about the physical delay.

Input measurement uses separate digital and crystal-clock measurements, with
bounded start/pop-up waits. Digital counters were observed to return one less
than total/active size on this board. The analog vertical count was 524 or 525
for the same source. The manual pp48–50 documents the fractional horizontal
measurement as a 16-line average and its four fractional bits in CR56.
Native/VGA must measure 31.3–31.7 kHz. CVT must measure 29.5–30.0 kHz and
accepts vertical counter endpoints 499 or 500. Matching resolution alone
never selects a profile. Each profile accepts all four measured HS/VS
polarities and normalizes capture from that measurement, rather than a
fixed polarity in the timing table. A polarity change in the same timing
profile blanks video and requires two matching samples before reacquisition.
Host tests cover all twelve mode/polarity combinations and retain the
geometry, totals, counter and rate rejection checks. Hardware qualification
of the newly accepted positive-polarity combinations is still pending.
The 2026-10-07 Metro P4/DDC upload verified every changed flash sector and
restored write protection. The RTD booted to its no-signal artwork, but the
post-reset live DDC CRC check failed; video qualification remains pending.

The input-status overlay reports geometry and timing independently of mode
acceptance. A successful digital measurement retains active dimensions and
horizontal total even when unsupported; a subsequent successful analog
measurement adds line frequency, vertical count and polarity. Validity flags
prevent timeout/overflow readings from appearing as current measurements.
Estimated refresh is line frequency divided by the measured vertical count,
rounded to 0.1 Hz; the observed 524/525 endpoint variation can change that
estimate by 0.1 Hz. These are measured values, not an EDID mode label.
The first failed acceptance check supplies the rejection reason. Existing
trace diagnostic codes and their three detail fields retain their meaning.

## Output clock

Manual pp131–135 describe DPLL divider encoding, charge-pump ratio, fine tuning,
and fixed-last-line controls. The UC-586 working configuration requires an
additional factor of two relative to the older manual's example. With N=8,
divisor=4, and the board's 27 MHz crystal, each M step is 421875 Hz. The code
chooses an integer M below the target and applies an upward fractional offset.
This empirical clock relationship is not a claim about every RTD266x part.

The clock routine accepts only 29.5–33 MHz, and mode application accepts only
the narrower range for its selected profile. All firmware arithmetic stays within 32 bits;
the host test compares clock recovery with a 64-bit reference calculation.

## Receiver and scaling

CR49[1:0]=00 selects TMDS on the tested chip, although the older RTD2660 manual
marks it reserved. Page 2 AB[1:0]=3 and B5[7]=1 are observed receiver settings;
their analog rationale remains unresolved. The UC-586 uses port 0 with both
differential polarity and red/blue lane swaps (page 2 A7=0x6F), selected by its
board configuration. Automatic HDMI/DVI detection is enabled; HDCP keys are not
provided. The separate [audio driver](audio.md) handles stereo LPCM.

Keep aspect is the default. Both supported widths have 480 active lines, so
both axes remain at 1:1: 640x480 is centered with 80 black pixels on each side,
and 800x480 fills the panel. Timing-port indices 0x05/0x07 bound the picture;
0x03/0x09 retain the full 800-pixel background/DE window (manual p33). Panel
totals and the OSD origin stay unchanged. Mode application sets the background
to black. Both bypassed axes receive 0xFFFFF; the full line buffer remains
enabled, and the downscaler and its auxiliary buffer are bypassed.
Host checks cover margins, unity factors, black background and transitions
among the VGA/native/CVT profiles. On 2026-09-29, the UC-586 displayed the HSTX
640x480 grid centered with black sidebars and both vertical red border lines
visible. Full 512 KiB readback matched and protection returned to 0x0C. This
bench check covered VGA; the native/CVT transitions were checked on the host.

The neutral filter is a triangular linear-interpolation kernel generated at startup:
for `p=0..15`, the stored tap weights are
`[0, 16+32*p, 1008-32*p, 0]`. Every phase sums to 1024; there are no negative
lobes at the neutral setting. The 4-tap/32-phase interpretation, half-phase alignment, and normalization
are inferences from numerical inspection of a working filter, not guarantees
in the manual. Earlier firmware used 0xCCCCD for 640/800 horizontal expansion;
a grid expanded to the full panel with its border visible in the first
fresh-firmware bench test. The keep-aspect default bypasses this filter.
Detailed filter response and color
fidelity have not been characterized.
The manual p41 specifies 64 stored 12-bit coefficients, low byte first, with
the other half supplied by symmetry. The new table is loaded into inactive
bank 2 for both color paths and selected only for horizontal filtering.

## Picture controls

Image brightness and contrast use common-page CR64/CR65 Set A, documented on
manual pp61-62. Each accepts 0..100, with 50 mapping to the neutral coefficient
128. Brightness applies the same signed offset to all channels. Red, green,
and blue gains independently multiply the corresponding contrast coefficient;
50 is unity, zero removes that channel, and 100 requests twice its gain. The
combined gain saturates at the hardware maximum of 255, so maximum contrast and
maximum channel gain cannot multiply into a fourfold hardware gain.

Saturation uses the common-page sRGB matrix, not the video-decoder chroma
register or a YUV-only peaking path. Manual pp60-61 defines its transform as
`I + C`, with signed coefficients and per-channel offsets. The original
Q8 matrix generator mixes toward luma weights `[77,150,29]/256`, an integer
approximation of BT.601. The transform produces greyscale at zero, bypasses the
matrix at 50, and doubles chroma displacement from luma at 100. Each matrix
row sums exactly to unity after integer rounding, preserving neutral grey.
It writes sign/high byte followed by low byte in a contiguous six-byte row,
clears the three offsets, and requests the DVS latch with CR62 bit7 while
retaining the blue-row selector. No inherited coefficient table is used.

UC-586 requires CR62[6]=1 and CR64[6]=1: the documented two-bit coefficient
left shift. The reference calculation retains Q10 through multiplication,
halves the final C, and enables one-bit enlargement. Together with the bench
result, this supports a 1024 multiplier denominator on the target silicon,
despite the manual's eight-fractional-bit description. Our Q8 coefficients
therefore need the fourfold shift. Brightness/contrast updates preserve both
precision bits. Neutral still bypasses the matrix.

The v40-v43 builds produced only partial desaturation at zero. A live v42
readback showed CR60=00, CR62=1F, CR64=00, CR68=01, and page7:D8=00, ruling
out highlight masking, disabled sRGB, and an unapplied DVS latch. Neither
latch-sequence alignment nor contiguous row writes resolved the weak effect.
The precision change in v44 did: on 2026-09-30, the UC-586 showed luminance
bars at saturation zero, reduced chroma at 25, and the original colors at 50.
The original saturated primaries clip at 100, so they cannot demonstrate
additional chroma gain; that endpoint's arithmetic is host-tested. Grey-ramp
captures at 0 and 100 preserved the broad luminance ramp and common panel/
camera color cast. These are functional image checks, not colorimetry.
DDC setting readbacks passed throughout the sweep. Evidence is in the bench
captures `fresh-v44-osd-controls/{sat0,bars-sat25,bars-sat50,bars-sat100,
gray-sat0,gray-sat100}.png` and `saturation-readback.json`.

The 0..100 sharpness control operates horizontally. At 50 it preserves the
existing linear filter and bypasses filtering for 1:1 display. Other settings
enable horizontal filtering even at unity geometry. The generated kernel
convolves linear interpolation with `[-a,1+2a,-a]`, where
`a=(sharpness-50)/200`: zero softens, 50 is neutral, and 100 applies a modest
unsharp mask. Every phase sums exactly to 1024 and fits signed 12-bit storage.
The inactive coefficient bank is written before the horizontal bank is
switched; receiver, PLL, frame timing, and vertical scaling remain unchanged.
The signed coefficient convention and phase layout were inferred from the
established scaler tables. On 2026-09-30, 4K captures of 640x480 expanded in
Fill mode showed softer edges at zero and modestly crisper edges at 100.
Detailed filter response and the visual effect at 1:1 remain uncharacterized.
Evidence is `fresh-v43-color-stream/fill-sharp0.png` and `fill-sharp100.png`.

Factoring four out of every interpolation slope keeps products within signed
16-bit range. A complete host sweep verifies every coefficient against the
original 32-bit formula, including truncation toward zero. A synchronous
upload still exceeded the DDC response window. Setters now only queue a
percentage; `video_controls_service()` writes
one coefficient into the inactive bank per call, and selects it after all 64
are complete. A new request restarts that bank without exposing a partial
filter. Startup drains the queue synchronously before DDC initialization;
the monitor services later changes even during signal loss. This needs only
two extra bytes of state and no coefficient buffer. The v42 bench passed
immediate DDC Gets with the normal 50 ms response window, including 15 reads
each after sharpness 0, 100, and 50.

Host tests sweep all saturation and sharpness values, verify grey/DC
preservation, signed encodings, inactive-bank writes, gain/contrast composition,
clamping, and unchanged geometry. These prove arithmetic and register writes,
not the physical image response or color calibration.

## Forced aspect ratios

The aspect API names Keep=0, Fill=1, 4:3=2, and 16:9=3. Callers query current
availability before accepting a user change. Unsupported or invalid choices
leave the previous selection intact, except that an enabled 16:9 build retains
a saved preference across incompatible sources and falls back to Keep.
`video_aspect_current()` reports the actual display mode, including this
fallback. Keep and Fill retain their established behavior.

`RTD_ASPECT_4_3=1` enables the horizontal downscaler and is the default.
An 800x480 source retains its complete capture window,
then UZD produces 640x480 for the FIFO and centered display. A 640x480 source
already has the requested shape and bypasses UZD. All three timing profiles
retain their existing pixel clock, full-panel DE raster, and frame-delay values.

Manual pp150-155 documents page 6 E3/E4 enable, RGB boundary selection, table
selection and buffer controls; E5-E7 use `input_width/output_width * 2^20`.
800/640 therefore uses 0x140000. Zero nonlinear delta and first segment leave
the entire output in the 640-pixel second segment. The auxiliary buffer stays
bypassed, which the manual explicitly permits for horizontal UZD. FIFO width
follows the post-downscale width (manual p37). Phase initialization uses the
vendor-observed `255 - input_width*255/output_width` convention, giving 193.

UZD's original four-tap filter convolves linear interpolation with
`[1/8,3/4,1/8]` for mild smoothing before subsampling. It has eight stored
half-phases, each summing to 1024. The 32 coefficients are written low byte
first in the manual's F3/F4 order, while UZD is disabled. Phase interpretation
and normalization are behavioral inferences from a working reference, not a
copied table. Native/CVT/VGA transitions and full-source capture are host-tested.
The UC-586 native 800x480 bench check confirmed complete-source downscaling and
alignment; forced 4:3 with the 500-line CVT source has not been physically tested.

`RTD_ASPECT_16_9=1` enables the qualified 525-line timing configuration and is
the default. Fitting the entire
source into an 800x450 picture requires vertical downscaling. With line-buffer
streaming, the reference driver changes display line rate in proportion to
output/input height; simply changing the picture height at the existing clock
would overrun or underrun buffering. A 500-line CVT frame at 450/480 line rate
has only 468.75 output lines, fewer than this panel's 480 active rows. A 525-line
source offers 492.1875 output lines and permits the following configuration:

- The previously matched display clock is multiplied by 15/16, giving
  29.53125 MHz for a nominal 31.5 MHz source-matched output clock. Availability
  requires the reduced clock to stay within the existing 29.5 MHz lower bound.
- The full 480-row background/DE raster runs from line 6 to 486; picture lines
  21 to 471 supply a complete 800x450 image with 15-row black bars. The DTG
  watchdog total is 493; actual DVS stays synchronized to the 525-line input.
- UZD vertical factor is `floor(480/450 * 2^20) = 0x111111`, with phase
  initializer 239 and H-to-V auxiliary buffer mode. FIFO height is 450; the
  capture remains all 480 source lines. VGA still expands horizontally to 800.
- The frame delay preserves each source mode's measured first-pixel lead after
  converting the old and new display origins into input-clock units. It adds
  one quarter input line because the reference vertical-downscale FIFO target
  is 1.75 lines versus 1.5 at unity. CR41 rounding is below 16 input clocks.

These shortened porches and delay adjustments were tested on the UC-586 with
both supported 525-line input widths. Host tests additionally check complete
capture, the 450/480 windows, DPLL
arithmetic, delay arithmetic, and restoration of every original raster on exit.
CVT or a too-low matched clock automatically uses Keep; returning to a compatible
source restores the saved 16:9 preference. No-input and signal-loss states
report Keep and restore the known startup raster. The driver never labels a
cropped image or an incompatible fallback as an active forced 16:9 picture.

On 2026-09-30, v44 displayed a complete 800x480 HSTX grid in Keep. Forced
4:3 preserved all 25 columns, 15 rows, and both red side borders inside the
centered 640-pixel picture, with 80-pixel black sidebars. Forced 16:9 retained
the full grid in the 800x450 letterboxed picture. Forced 4:3 returned after
source off/on. The earlier 640x480 check also preserved the complete grid in
16:9 and returned correctly to Keep. Evidence is in
`fresh-v44-osd-controls/800-keep-grid.png`, `800-aspect43-grid.png`,
`800-aspect169-grid.png`, and `800-aspect43-return.png`; these qualify the
video paths, not audio at the new source timing. Detailed resampling quality
has not been characterized.

## Startup and missing input

The panel starts in free-running mode with CR28 bit7 forcing the timing
generator on, bit5 selecting the full-screen background, and bit3 clear
(manual pp30–31). This raster does not depend on input VSYNC. The application
shows its bitmap over that background for one second after uploading the tiles,
then starts measuring video.
Mode application configures capture and scaling before selecting frame sync
and revealing input pixels. Signal loss returns to the black free-running
background and shows the independent no-signal bitmap. That asset is uploaded
only on the transition to missing input and hidden after successful acquisition.
Leaving the short CVT raster restores the panel's default 525-line timing,
32-line active origin and 31.5 MHz clock. OSD positioning reads the actual output
origin, so its top-left status box remains aligned in either raster. The app
qualifies two matching profile IDs, including switches between the two 800x480
timings; it does not retune the PLL on every small measured clock fluctuation.

## Bench diagnostics

With `TRACE=1`, EDID bytes 95–107 contain one hexadecimal error digit followed
by three four-digit hexadecimal fields. The checksum is updated before external
DDC access is re-enabled. `include/rtd/video.h` defines the error codes and field
meanings. For example, `03581020C0000` means success, a 16-line period count of
13697, 524 vertical lines, and negative H/V sync. Timeout fields may be stale.

MCU scratch register `0xff19` receives the error code; `0xfff2` receives the
ASCII error character read back from EDID RAM. Read those through ISP only.
An ISP read pauses and restarts the MCU, so it is not a passive live trace.
Normal builds instead leave startup/acquisition stage markers in those bytes.

## Deliberate limits

- No general input timing solver or support for arbitrary refresh rates.
- No automatic DPLL rewrite on every measured clock fluctuation. The app should
  reacquire on loss or mode change; repeated fine-clock changes previously
  destabilized a working display.
- No claim that the generic RGB888 panel assumptions fit other boards.
- Host tests cover register encoding and rejection behavior, not analog lock,
  the filter's physical mapping, color fidelity, or thermal/electrical limits.
