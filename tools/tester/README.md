# RTD266x Feather tester and programmer

The tester sketches, shared RTD266x ISP driver and Python host CLI are maintained
here alongside the display firmware. A checkout of this repository contains
both sides of the programming connection.

- [Feather RP2350 HSTX](feather_rp2350/Feather_HSTX_RTD_Tester/README.md):
  640x480 or 800x480 HDMI video with a 48 kHz audio test tone, live DDC/CI controls and flash
  programming through the same HDMI cable and Adafruit HSTX-to-DVI adapter.
- [Feather RP2040 DVI](feather_rp2040/Feather_DVI_RTD_Tester/README.md):
  640x480 and 800x480 video patterns, plus flash programming.
- [Metro P4](metro_p4/README.md): flash programming through the LT8912B
  adapter's DDC switch while holding that bridge in reset; no video.

All use `feather_rp2040/Feather_DVI_RTD_Tester/host.py` and the single
`RTD266xISP.cpp/.h` implementation in that directory. The HSTX sketch includes
the shared driver through relative wrappers; follow its documented compile
command. Install the Python dependency with `pip3 install pyserial`.

From this repository's root:

```sh
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py info
```

Use `--port` or `--serial` before the command to select a Feather when needed.
Programming requires video off, a matching verified backup and explicit
`--allow-write`; follow the full backup/recovery instructions in the RP2040
tester's README. The enabled flash profile is W25X40, JEDEC EF3013, 512 KiB.
After programming, explicitly run `reset-chip` while still in `mode off`.
An MCU-only restart retained DDC state on the UC-586; whole-chip reset cleared it.

The RP2350 sketch also supports `vcp-get`, `vcp-set`, `key` and `menu-state`
through the shared host CLI, in either video mode or off mode. These live
transactions use address `0x37` without entering ISP. For example:

```sh
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py key menu
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py vcp-set 0x8d 1
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py vcp-set 0x8d 2
```

The last two commands mute and unmute. See the [firmware control map](../../docs/ddcci.md)
for supported codes, EEPROM settings storage and validation limits. The RP2040
tester does not yet implement the live DDC transport.

## Firmware CRC and faster bank0 updates

With the HSTX tester and CRC-capable Adafruit firmware already running:

```sh
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py firmware-crc firmware.bin
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py firmware-crc current-full.bin --region vendor-probe
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py mode off
# Wait for the Feather's USB port to reconnect before programming.
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py program updated-full.bin --backup current-full.bin --allow-write --fast
```

`firmware-crc` defaults to `--region bank0` and accepts a 65,536-byte bank0
image or a complete 524,288-byte image. It compares the firmware's live CRC32
against Python `zlib.crc32` of the first 65,536 bytes, using DDC without entering
ISP. `--region vendor-probe` requires the complete 524,288-byte image and checks
only its 8,192 bytes at `0x010000..0x011FFF`: the first two retained vendor
sectors, read by the running firmware through its mapped XDATA window. It does
not check the rest of the retained tail.

Each command starts one job, polls for up to 120 seconds, requires an observed
busy state after starting and confirms the completed job's region ID before
accepting its result. The JSON receipt names
the region, start address, size, CRC and SHA256 of the expected region bytes;
`verified` applies only to that scope. Neither command verifies the full flash
or settings EEPROM. Unsupported firmware, an already-busy job, wrong region,
transport errors, timeout or a mismatch fail explicitly without automatic retry.

`program --fast` always checks bank0, even after a vendor-probe job, and still
requires video off and full-size backup/target files.
It requires W25X40 (`EF3013`) with whole-flash protection already enabled
(`status & 0x1c == 0x1c`); partial protection such as `0x0c` is rejected before
unlock or writes. Use normal full verification and restore full protection
before using `--fast` on a partially protected device.
For the UC-586 fresh firmware, the existing guarded command can establish this
policy: `host.py restore-protection current-full.bin --status 0x1c --allow-write
--receipt protection.json`. It checks the entire current image before changing
only the flash protection bits. Do this before the first fresh-firmware install;
normal programming then restores that protection before releasing the MCU.
The target and backup bytes after bank0 must be identical. A live CRC must first
identify current bank0 as the supplied backup or target; then ISP reads and exactly
compares all 64 KiB of bank0 before any erase. Only changed bank0 sectors are
written, and each is read back. The command restores protection, exits ISP,
performs `reset-chip`, waits three seconds for startup and checks the running
target bank's CRC. The receipt records `verification_scope: bank0`,
`full_image_verified: false` and `tail_readback: false`; the tail is trusted from
the supplied verified backup and is neither read nor changed in this operation.

The current and target firmware must both support this CRC interface. Use normal
`program` for an initial installation, an unsupported firmware or a changed tail.
`--fast` cannot be combined with `--recover`. Normal programming and recovery
retain their full-image preflight/readback behavior. Do not substitute this
bank0-only workflow for the original matching full backups.

Run the host-only regression tests without connected hardware:

```sh
python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/test_firmware_crc.py
```

Imported from Adafruit_Arduino_Tester_Code commit
[`0511f0f`](https://github.com/adafruit/Adafruit_Arduino_Tester_Code/commit/0511f0f1d34d7b2bfbf8b580b19ed6e7d6655b74).
Future RTD tester and ISP-driver changes belong in this firmware repository.
