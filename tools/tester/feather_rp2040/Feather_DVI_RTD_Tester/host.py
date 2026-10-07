#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Adafruit Industries
# SPDX-License-Identifier: MIT
"""USB host for Feather DVI video tests and RTD266x flash recovery.

Install pyserial, then run `python host.py --help`. Flash commands require the
display to have its own power and an HDMI cable to the Feather. DDC is not video.
This module can also be imported: Client.command() exposes the firmware protocol
for a controlled hardware test. Closing Client does not leave ISP or reboot the
display; callers of the low-level interface must explicitly finish a safe image.
"""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import zlib

import serial
from serial.tools import list_ports


DEFAULT_SERIAL = None
SECTOR_SIZE = 4096
PAGE_SIZE = 256
BANK_SIZE = 65536
FULL_IMAGE_SIZE = 512 * 1024
CRC_REGIONS = {"bank0": (1, 0, BANK_SIZE),
               "vendor-probe": (2, BANK_SIZE, 8192)}
PATTERNS = ("bars", "checker", "red", "green", "blue", "gray", "black",
            "white", "grid", "text")
VIRTUAL_KEYS = {"menu": 1, "back": 2, "up": 4, "down": 8, "power": 16}
DDC_CI_GAP_SECONDS = 0.05


class TesterError(Exception):
    """A failed protocol operation, validation, or safety prerequisite."""


def timestamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def select_port(port=None, usb_serial=DEFAULT_SERIAL):
    if port:
        return port
    candidates = [p for p in list_ports.comports()
                  if p.vid == 0x239A and p.pid in (0x8127, 0x814F)
                  and (not usb_serial or p.serial_number == usb_serial)]
    if len(candidates) != 1:
        found = ", ".join(p.device for p in candidates) or "none"
        raise TesterError("Expected one Feather DVI USB serial device; found "
                          + found + ". Use --port or --serial.")
    return candidates[0].device


class Client:
    """One outstanding text command at a time; mutations are never retried."""

    def __init__(self, port=None, usb_serial=DEFAULT_SERIAL):
        self.port = select_port(port, usb_serial)
        native_esp_usb = any(p.device.casefold() == self.port.casefold()
                             and p.vid == 0x303A and p.pid == 0x1001
                             for p in list_ports.comports())
        if native_esp_usb:
            # Native ESP USB Serial/JTAG uses DTR/RTS for reset. Set their
            # states before opening so reconnecting keeps the programmer alive.
            self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                                        write_timeout=2)
            self.serial.dtr = False
            self.serial.rts = False
            self.serial.port = self.port
            self.serial.open()
        else:
            self.serial = serial.Serial(self.port, 115200, timeout=0.1,
                                        write_timeout=2)
        self.buffer = bytearray()
        self.synchronized = True

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self.serial.close()

    def command(self, command, timeout=5):
        if not self.synchronized:
            raise TesterError("USB response state is uncertain; close and reconnect "
                              "before issuing any further command")
        if "\n" in command or "\r" in command:
            raise TesterError("A command must be a single line")
        packet = (command + "\n").encode("ascii")
        self.synchronized = False
        if self.serial.write(packet) != len(packet):
            raise TesterError("Incomplete USB serial write; command not retried")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" not in self.buffer:
                self.buffer.extend(self.serial.read(
                    max(1, min(self.serial.in_waiting, 4096))))
                if len(self.buffer) > 16384:
                    raise TesterError("Oversized response; connection is unsafe to reuse")
                continue
            line, _, tail = self.buffer.partition(b"\n")
            self.buffer = bytearray(tail)
            if not line.strip():
                continue
            try:
                response = json.loads(line)
            except (ValueError, UnicodeDecodeError) as error:
                raise TesterError("Non-JSON response: " + repr(bytes(line[:160]))) from error
            if not isinstance(response, dict):
                raise TesterError("Expected a JSON object response")
            # Firmware may announce itself when USB opens. Do not mistake its
            # welcome object for a scan, read, or write acknowledgement.
            if (response.get("firmware") == "FeatherDVI-RTD"
                    and (response.get("event") == "ready" or command != "hello")):
                continue
            if "ok" not in response:
                raise TesterError("Missing command acknowledgement")
            self.synchronized = True
            if response.get("ok") is not True:
                raise TesterError(str(response.get("error", "Missing ok:true")))
            return response
        raise TesterError("Timeout for " + command.split()[0]
                          + "; command not retried, connection state uncertain")

    def hello(self, require_isp=False):
        response = self.command("hello")
        if (response.get("firmware") != "FeatherDVI-RTD"
                or response.get("protocol") != 1):
            raise TesterError("Wrong firmware or unsupported protocol")
        if require_isp and type(response.get("isp_active")) is not bool:
            raise TesterError("Firmware did not report a valid ISP state; "
                              "no automatic ISP exit or reset is safe")
        return response

    def isp(self):
        response = self.command("isp")
        jedec = response.get("jedec", "")
        size = response.get("size")
        try:
            valid_id = len(jedec) == 6 and len(bytes.fromhex(jedec)) == 3
        except (TypeError, ValueError):
            valid_id = False
        if not valid_id or not isinstance(size, int) or isinstance(size, bool):
            raise TesterError("Invalid flash identification response")
        if size < SECTOR_SIZE or size > 32 * 1024 * 1024 or size & (size - 1):
            raise TesterError("Invalid flash capacity")
        if (not isinstance(response.get("status"), int)
                or not 0 <= response["status"] <= 255):
            raise TesterError("Expected one flash status byte")
        return response

    def read(self, address, length):
        if address < 0 or not 1 <= length <= PAGE_SIZE:
            raise TesterError("Read requires a nonnegative address and 1..256 bytes")
        response = self.command(f"read {address} {length}", timeout=5)
        if response.get("address") != address:
            raise TesterError("Read returned the wrong address")
        return hex_data(response, length)

    def read_flash(self, size, address=0, progress=None):
        result = bytearray()
        for offset in range(0, size, PAGE_SIZE):
            result.extend(self.read(address + offset, min(PAGE_SIZE, size - offset)))
            if progress and (len(result) % 65536 == 0 or len(result) == size):
                progress(len(result), size)
        return bytes(result)

    def ddc_write(self, packet):
        """Write one live DDC/CI packet; the HSTX tester keeps video running."""
        if not isinstance(packet, bytes) or not 1 <= len(packet) <= 32:
            raise TesterError("DDC write requires 1..32 bytes")
        response = self.command(f"ddc {packet.hex()} 0")
        # The host waits while the Feather resumes audio and the RTD parses.
        time.sleep(DDC_CI_GAP_SECONDS)
        if response.get("written") != len(packet):
            raise TesterError("DDC write returned the wrong byte count; not retried")

    def ddc_read(self, length):
        """Read once, then allow the RTD FIFO to return to receiving requests."""
        if type(length) is not int or not 1 <= length <= 32:
            raise TesterError("DDC read requires a length of 1..32 bytes")
        response = self.command(f"ddc - {length}")
        time.sleep(DDC_CI_GAP_SECONDS)
        return hex_data(response, length)


def hex_data(response, length):
    encoded = response.get("data")
    if not isinstance(encoded, str) or len(encoded) != length * 2:
        raise TesterError("Wrong response data length")
    try:
        result = bytes.fromhex(encoded)
    except ValueError as error:
        raise TesterError("Response is not valid hexadecimal") from error
    if len(result) != length:
        raise TesterError("Wrong decoded data length")
    return result


def progress(label):
    def report(done, size):
        print(f"{label}: {done}/{size} bytes", file=sys.stderr, flush=True)
    return report


def first_difference(expected, actual):
    for address, (left, right) in enumerate(zip(expected, actual)):
        if left != right:
            return address
    return min(len(expected), len(actual))


def compare(expected, actual, label="Flash", base=0):
    if expected != actual:
        offset = base + first_difference(expected, actual)
        raise TesterError(f"{label} differs at 0x{offset:06X}; "
                          f"expected SHA256 {sha256(expected)}, read {sha256(actual)}")


def finish(client):
    # No automatic retry: a missing reply is not proof that a command failed.
    # Firmware restores protection, exits ISP and resets the DW8051 itself.
    client.command("finish", timeout=10)


def read_session(client, action):
    was_active = client.hello(require_isp=True)["isp_active"]
    entered = False
    try:
        info = client.isp()
        entered = True
        info["isp_active_at_entry"] = was_active
        result = action(info)
    except Exception as error:
        if was_active or not entered or not client.synchronized:
            raise TesterError(f"{error}; finish/reset NOT attempted. "
                              "Pre-existing or unconfirmed ISP state preserved.") from error
        try:
            finish(client)
        except Exception as cleanup:
            raise TesterError(f"{error}; ISP cleanup also failed: {cleanup}. "
                              "Protection/ISP state is unconfirmed.") from error
        raise
    else:
        if was_active:
            print("ISP was active at entry; left active without finish/reset.",
                  file=sys.stderr)
        else:
            finish(client)
        return result


def metadata_path(path):
    return path.with_name(path.name + ".json")


def require_new(path, include_metadata=True):
    targets = [path, metadata_path(path)] if include_metadata else [path]
    for target in targets:
        if target.exists():
            raise TesterError("Refusing to overwrite " + str(target))
    if not path.parent.is_dir():
        raise TesterError("Output directory does not exist: " + str(path.parent))


def write_json_new(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, indent=2)
        output.write("\n")


def save_binary(path, data, metadata):
    require_new(path)
    with path.open("xb") as output:
        output.write(data)
    metadata.update({"file": path.name, "size": len(data), "sha256": sha256(data),
                     "created_utc": timestamp()})
    write_json_new(metadata_path(path), metadata)


def read_image(path):
    data = path.read_bytes()
    sidecar = metadata_path(path)
    if sidecar.exists():
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        recorded = metadata.get("sha256")
        if recorded is not None and (not isinstance(recorded, str)
                                     or recorded.lower() != sha256(data)):
            raise TesterError("Image does not match its SHA256 metadata: " + str(path))
    return data


def detailed_timing(data):
    clock = int.from_bytes(data[:2], "little") * 10000
    if not clock:
        return None
    horizontal = data[2] + ((data[4] & 0xF0) << 4)
    hblank = data[3] + ((data[4] & 0x0F) << 8)
    vertical = data[5] + ((data[7] & 0xF0) << 4)
    vblank = data[6] + ((data[7] & 0x0F) << 8)
    hfront = data[8] + ((data[11] & 0xC0) << 2)
    hsync = data[9] + ((data[11] & 0x30) << 4)
    vfront = (data[10] >> 4) + ((data[11] & 0x0C) << 2)
    vsync = (data[10] & 0x0F) + ((data[11] & 0x03) << 4)
    total = (horizontal + hblank) * (vertical + vblank)
    result = {"pixel_clock_hz": clock, "width": horizontal, "height": vertical,
            "horizontal_blanking": hblank, "vertical_blanking": vblank,
            "horizontal_front_porch": hfront, "horizontal_sync": hsync,
            "horizontal_back_porch": hblank - hfront - hsync,
            "vertical_front_porch": vfront, "vertical_sync": vsync,
            "vertical_back_porch": vblank - vfront - vsync,
            "refresh_hz": round(clock / total, 5) if total else None,
            "interlaced": bool(data[17] & 0x80), "flags": data[17]}
    if result["interlaced"]:
        # EDID encodes vertical active/blanking/porches per field. The two
        # fields add one half-line each, making the complete frame total odd.
        # Keep the encoded porches explicit rather than assigning that extra
        # half-line to a porch that the descriptor does not specify.
        frame_lines = 2 * (vertical + vblank) + 1
        frame_pixels = (horizontal + hblank) * frame_lines
        frame_rate = clock / frame_pixels if frame_pixels else None
        result.update({
            "height": 2 * vertical,
            "vertical_blanking": 2 * vblank + 1,
            "encoded_vertical_per_field": {
                "active_lines": vertical, "blanking_lines": vblank,
                "front_porch_lines": result.pop("vertical_front_porch"),
                "sync_lines": result.pop("vertical_sync"),
                "back_porch_lines": result.pop("vertical_back_porch"),
            },
            "additional_half_line_per_field": 0.5,
            "field_total_lines": frame_lines / 2,
            "frame_total_lines": frame_lines,
            "field_rate_hz": round(2 * frame_rate, 5) if frame_rate else None,
            "frame_rate_hz": round(frame_rate, 5) if frame_rate else None,
            "refresh_hz": round(2 * frame_rate, 5) if frame_rate else None,
        })
    return result


def decode_edid(blocks):
    base = blocks[0]
    manufacturer = int.from_bytes(base[8:10], "big")
    result = {
        "header_valid": base[:8] == bytes.fromhex("00ffffffffffff00"),
        "manufacturer": "".join(chr(64 + ((manufacturer >> shift) & 31))
                                for shift in (10, 5, 0)),
        "product_code": int.from_bytes(base[10:12], "little"),
        "serial_number": int.from_bytes(base[12:16], "little"),
        "version": f"{base[18]}.{base[19]}", "extension_count": base[126],
        "checksums_valid": [sum(block) % 256 == 0 for block in blocks],
        "detailed_timings": [],
    }
    for offset in range(54, 126, 18):
        descriptor = base[offset:offset + 18]
        timing = detailed_timing(descriptor)
        if timing:
            result["detailed_timings"].append({"block": 0, **timing})
        elif descriptor[:3] == b"\0\0\0" and descriptor[3] in (0xFC, 0xFF):
            key = "display_name" if descriptor[3] == 0xFC else "display_serial"
            result[key] = descriptor[5:18].decode("ascii", errors="replace").strip()
    for number, block in enumerate(blocks[1:], 1):
        if block[0] == 2 and 4 <= block[2] <= 109:
            for offset in range(block[2], 110, 18):
                timing = detailed_timing(block[offset:offset + 18])
                if timing:
                    result["detailed_timings"].append({"block": number, **timing})
    return result


def dump_flash(client, output, reads=2):
    require_new(output)
    if reads < 2:
        raise TesterError("A backup requires at least two independent reads")

    def action(info):
        first = None
        hashes = []
        for index in range(reads):
            data = client.read_flash(info["size"], progress=progress(f"Read {index + 1}"))
            hashes.append(sha256(data))
            if first is None:
                first = data
            else:
                compare(first, data, "Repeated backup reads")
        return first, {"operation": "dump", "flash": info, "read_sha256": hashes,
                       "matching_reads": reads, "port": client.port}

    data, metadata = read_session(client, action)
    save_binary(output, data, metadata)
    return metadata


def verify_flash(client, image):
    expected = read_image(image)

    def action(info):
        if len(expected) != info["size"]:
            raise TesterError("Image size does not match identified flash capacity")
        actual = client.read_flash(info["size"], progress=progress("Verify"))
        compare(expected, actual)
        return {"operation": "verify", "verified": True, "flash": info,
                "size": len(actual), "sha256": sha256(actual), "created_utc": timestamp()}

    return read_session(client, action)


def ddc_config(client):
    """Read the fixed whitelist with the existing ISP ownership safeguards."""
    def action(info):
        response = client.command("ddc-config")
        registers = response.get("registers")
        channels = response.get("channel_access")
        expected = {"DDC_RAM_PARTITION", "PIN_SHARE_CONTROL14", "WDT_CONTROL",
                    "ISP_SLAVE_ADDRESS", "ISP_MCU_CONTROL", "ISP_MCU_CLOCK_CONTROL",
                    "BANK_CONTROL", "BANK_XDATA_START", "BANK_XDATA_SELECT",
                    "BANK_PBANK_SWITCH", "REV_DUMMY2", "REV_DUMMY6"}
        expected.update(f"DDC{channel}_CONTROL{control}"
                        for channel in range(1, 4) for control in range(3))
        if not isinstance(registers, dict) or set(registers) != expected:
            raise TesterError("Incomplete DDC configuration register snapshot")
        if not isinstance(channels, dict) or set(channels) != {"FFEC", "FFED"}:
            raise TesterError("Incomplete DDC channel-access snapshot")
        for value in list(registers.values()) + list(channels.values()):
            hex_data({"data": value}, 1)
        response["isp_active_at_entry"] = info["isp_active_at_entry"]
        return response

    return read_session(client, action)


def program_flash(client, target, backup, recover=False, fast=False):
    """Program only changed sectors. On any write failure, stay in ISP."""
    if fast and recover:
        raise TesterError("--fast cannot be combined with --recover; use full verification")
    if fast and (len(target) != FULL_IMAGE_SIZE or len(backup) != FULL_IMAGE_SIZE):
        raise TesterError("Fast programming requires full 512 KiB target and backup images")
    if fast and target[BANK_SIZE:] != backup[BANK_SIZE:]:
        raise TesterError("Fast programming requires identical target/backup tails after bank0")
    if recover and target != backup:
        raise TesterError("Recovery target must exactly equal the verified backup; "
                          "finish/reset NOT attempted")
    hello = client.hello(require_isp=True)
    if hello.get("video") != "off":
        raise TesterError("Programming requires video off; use 'mode off' first "
                          "and reconnect. ISP/finish/reset NOT attempted.")
    was_active = hello["isp_active"]
    preflight_crc = None
    if fast:
        if was_active:
            raise TesterError("Fast programming requires live firmware with ISP inactive; "
                              "use full verification without --fast")
        try:
            preflight_crc = read_firmware_crc(client, region="bank0")
        except TesterError as error:
            raise TesterError("Fast CRC preflight failed before ISP: " + str(error) +
                              "; use full verification without --fast") from error
        expected_crcs = {zlib.crc32(backup[:BANK_SIZE]), zlib.crc32(target[:BANK_SIZE])}
        if int(preflight_crc["crc32"], 16) not in expected_crcs:
            raise TesterError("Live bank0 CRC matches neither backup nor target; "
                              "ISP/erase/program NOT attempted")
    entered = False
    write_started = False
    failed_sector = None
    info = None
    try:
        info = client.isp()
        entered = True
        info["isp_active_at_entry"] = was_active
        if len(target) != info["size"] or len(backup) != info["size"]:
            raise TesterError("Target and backup must exactly match the flash capacity")
        if fast and (info["jedec"].upper() != "EF3013" or info["status"] & 0x1c != 0x1c):
            raise TesterError("Fast programming requires W25X40 (EF3013) with whole-flash "
                              "protection (status & 0x1c == 0x1c). Use normal full "
                              "verification and restore full protection before --fast; "
                              "unlock/erase/program NOT attempted")
        verify_size = BANK_SIZE if fast else info["size"]
        current = client.read_flash(verify_size, progress=progress("Preflight bank0" if fast else "Preflight"))
        if not recover and current != backup[:verify_size] and current != target[:verify_size]:
            raise TesterError("Current flash matches neither backup nor target. "
                              "For a partial failed image, use --recover with "
                              "the verified backup as both image and --backup.")
        sectors = [address for address in range(0, verify_size, SECTOR_SIZE)
                   if current[address:address + SECTOR_SIZE]
                   != target[address:address + SECTOR_SIZE]]
        pages_written = 0
        if sectors:
            client.command("arm " + info["jedec"].upper())
            client.command("unlock", timeout=10)
        for address in sectors:
            failed_sector = address
            write_started = True  # An erase may succeed even if its reply is lost.
            print(f"Programming sector 0x{address:06X}", file=sys.stderr, flush=True)
            client.command(f"erase {address}", timeout=30)
            sector = target[address:address + SECTOR_SIZE]
            for offset in range(0, SECTOR_SIZE, PAGE_SIZE):
                page = sector[offset:offset + PAGE_SIZE]
                if page != b"\xff" * PAGE_SIZE:
                    client.command(f"page {address + offset} {page.hex()}", timeout=30)
                    pages_written += 1
            actual = client.read_flash(SECTOR_SIZE, address=address)
            compare(sector, actual, "Programmed sector", base=address)
        if not fast:
            actual = client.read_flash(info["size"], progress=progress("Full readback"))
            compare(target, actual, "Final full readback")
    except Exception as error:
        if write_started:
            raise TesterError(
                f"Programming failed at/after sector 0x{failed_sector:06X}: {error}. "
                "ISP LEFT ACTIVE; protection restoration and reboot NOT attempted. "
                "Keep display powered; recover the image from the verified backup "
                "before issuing finish/reset.") from error
        if recover or was_active or not entered or not client.synchronized:
            raise TesterError(f"{error}; finish/reset NOT attempted. "
                              "Recovery, pre-existing or unconfirmed ISP state "
                              "preserved.") from error
        try:
            finish(client)
        except Exception as cleanup:
            raise TesterError(f"{error}; ISP cleanup also failed: {cleanup}. "
                              "Protection/ISP state is unconfirmed.") from error
        raise
    # Full mode compares every byte. Fast mode preserves the trusted tail and
    # verifies bank0's preflight plus every changed sector before booting.
    try:
        finish(client)
    except Exception as error:
        raise TesterError("Image verified, but protection restoration/ISP exit/reset "
                          "was not fully confirmed: " + str(error)) from error
    final_crc = None
    if fast:
        try:
            client.command("reset-chip", timeout=15)
            # Allow both startup and no-signal artwork to finish. Timing
            # builds add a decode-only pass that deliberately does not
            # service DDC, so starting CRC mid-boot can leave a stale reply.
            time.sleep(10.0)
            final_crc = firmware_crc(client, target, region="bank0")
        except Exception as error:
            raise TesterError("Bank0 ISP checks and protection restoration completed, "
                              "but post-reset live CRC verification failed: " + str(error) +
                              "; no additional writes or reset attempted") from error
    result = {"operation": "program", "recovery": recover, "fast": fast,
            "verified": True, "verification_scope": "bank0" if fast else "full-flash",
            "verified_bytes": BANK_SIZE if fast else len(target),
            "full_image_verified": not fast, "tail_readback": not fast, "flash": info,
            "backup_sha256": sha256(backup), "sha256": sha256(target),
            "size": len(target), "changed_sectors": sectors,
            "pages_written": pages_written, "writes_skipped": not sectors,
            "protection_restored_and_reset": True, "created_utc": timestamp()}
    if fast:
        result.update({"preflight_firmware_crc": preflight_crc,
                       "final_firmware_crc": final_crc,
                       "tail_basis": "unchanged from supplied verified backup; not read"})
    return result


def restore_protection(port, usb_serial, image, status, receipt):
    """Recover a lost RAM protection setting only after exact full-image proof."""
    require_new(receipt, include_metadata=False)
    result = {"operation": "restore-protection", "image": str(image),
              "image_sha256": None, "before_status": None, "target_status": status,
              "verified": False, "protection_write_attempted": False,
              "finish_attempted": False, "protection_restored_and_reset": False}
    try:
        expected = read_image(image)
        result.update({"image_sha256": sha256(expected), "size": len(expected)})
        if len(expected) != 512 * 1024:
            raise TesterError("Protection recovery requires an exact 512 KiB image")
        if type(status) is not int or not 0 <= status <= 255 or status & 3 or not status & 0x1c:
            raise TesterError("Status must be a byte with nonzero BP bits and WIP/WEL clear")
        with Client(port, usb_serial) as client:
            result["port"] = client.port
            hello = client.hello(require_isp=True)
            if hello.get("video") != "off":
                raise TesterError("Protection recovery requires video off; use 'mode off' "
                                  "first and reconnect")
            result["isp_active_at_entry"] = hello["isp_active"]
            info = client.isp()
            result.update({"flash": info, "before_status": info["status"]})
            if info["jedec"].upper() != "EF3013" or info["size"] != len(expected):
                raise TesterError("Protection recovery requires Winbond W25X40 EF3013")
            if (status ^ info["status"]) & 0xe0:
                raise TesterError("Only BP bits may change; preserve current TB/SRP/reserved bits")
            actual = client.read_flash(info["size"], progress=progress("Recovery verify"))
            compare(expected, actual, "Protection recovery full image")
            result.update({"verified": True, "read_sha256": sha256(actual)})
            client.command("arm " + info["jedec"].upper())
            result["protection_write_attempted"] = True
            client.command(f"restore-protection {status}", timeout=10)
            confirmed = client.isp()
            result["after_status"] = confirmed["status"]
            if (confirmed["jedec"].upper() != info["jedec"].upper()
                    or confirmed["size"] != info["size"]
                    or confirmed["status"] & 0xfc != status & 0xfc
                    or confirmed["status"] & 3):
                raise TesterError("Protection readback does not match requested stable status")
            result["finish_attempted"] = True
            finish(client)
            result["protection_restored_and_reset"] = True
    except (Exception, KeyboardInterrupt) as error:
        result["error"] = str(error) or type(error).__name__
        if isinstance(error, KeyboardInterrupt):
            raise
        raise TesterError(f"{error}; no additional finish/reset attempted. "
                          "Keep the display powered and inspect the recovery receipt.") from error
    finally:
        result["created_utc"] = timestamp()
        write_json_new(receipt, result)
    result["receipt"] = str(receipt)
    return result


def ddc_checksum(data, seed):
    """VESA DDC/CI XOR: request includes 0x6e; reply substitutes host 0x50."""
    value = seed
    for byte in data:
        value ^= byte
    return value


def ddc_packet(payload):
    if not isinstance(payload, bytes) or not 1 <= len(payload) <= 29:
        raise TesterError("DDC payload must fit a 32-byte transaction")
    # Host source address, protocol length flag, command payload, checksum.
    packet = bytes((0x51, 0x80 | len(payload))) + payload
    return packet + bytes((ddc_checksum(packet, 0x6e),))


def vcp_get(client, code):
    if type(code) is not int or not 0 <= code <= 255:
        raise TesterError("VCP code must be 0..255")
    client.ddc_write(ddc_packet(bytes((0x01, code))))
    reply = client.ddc_read(11)
    if reply[:2] == b"\x6e\x80" and ddc_checksum(reply[:3], 0x50) == 0:
        raise TesterError("Display returned a DDC/CI null response; no VCP value available")
    if ddc_checksum(reply, 0x50) != 0:
        raise TesterError("DDC/CI reply checksum mismatch")
    if reply[:3] != b"\x6e\x88\x02":
        raise TesterError("Invalid DDC/CI Get VCP reply header or length")
    if reply[4] != code:
        raise TesterError("DDC/CI reply contains the wrong VCP code")
    if reply[3] != 0:
        raise TesterError(f"Display rejected VCP 0x{code:02X}, result 0x{reply[3]:02X}")
    if reply[5] not in (0, 1):
        raise TesterError("Invalid DDC/CI VCP type")
    return {"operation": "vcp-get", "code": f"0x{code:02X}", "type": reply[5],
            "maximum": int.from_bytes(reply[6:8], "big"),
            "current": int.from_bytes(reply[8:10], "big"), "reply": reply.hex()}


def vcp_set(client, code, value):
    if type(code) is not int or not 0 <= code <= 255:
        raise TesterError("VCP code must be 0..255")
    if type(value) is not int or not 0 <= value <= 65535:
        raise TesterError("VCP value must be 0..65535")
    packet = ddc_packet(bytes((0x03, code)) + value.to_bytes(2, "big"))
    client.ddc_write(packet)
    # Reset reapplies several hardware blocks. Let it finish before another
    # Set/Get pair can accumulate in the monitor's small receive FIFO.
    if code == 0x04 and value == 1:
        time.sleep(1.0)
    # Set VCP has no application-level response. Report only the bus ACK;
    # use Get VCP (or menu-state after a key) to observe the resulting state.
    return {"operation": "vcp-set", "code": f"0x{code:02X}", "value": value,
            "i2c_acknowledged": True, "request": packet.hex()}


def read_firmware_crc(client, timeout=120, region="bank0"):
    """Start one scoped CRC job; never retry failed transfers or restart jobs."""
    if region not in CRC_REGIONS:
        raise TesterError("Unknown firmware CRC region; use bank0 or vendor-probe")
    region_id, address, size = CRC_REGIONS[region]
    total_pages = size // PAGE_SIZE
    status = vcp_get(client, 0xf6)
    if status["maximum"] != 2 or status["current"] not in (0, 1, 2):
        raise TesterError("Firmware CRC interface returned an invalid status")
    if status["current"] == 1:
        raise TesterError("Firmware CRC job is already busy; no new job started")
    vcp_set(client, 0xf6, region_id)
    deadline = time.monotonic() + timeout
    previous_pages = 0
    observed_busy = False
    while time.monotonic() < deadline:
        status = vcp_get(client, 0xf6)
        if status["maximum"] != 2 or status["current"] not in (1, 2):
            raise TesterError("Firmware CRC job became idle or returned an invalid status")
        if status["current"] == 1:
            observed_busy = True
        elif not observed_busy:
            raise TesterError("Firmware CRC new job not observed after start; "
                              "ready result may be stale, job not retried")
        progress_value = vcp_get(client, 0xf9)
        pages = progress_value["current"]
        if (progress_value["maximum"] != total_pages or
                not previous_pages <= pages <= total_pages):
            raise TesterError("Firmware CRC returned invalid or decreasing page progress")
        previous_pages = pages
        if status["current"] == 2:
            if pages != total_pages:
                raise TesterError(f"Firmware CRC reported ready before all {total_pages} {region} pages")
            actual_region = vcp_get(client, 0xfa)
            if actual_region["maximum"] != 2 or actual_region["current"] != region_id:
                raise TesterError(f"Firmware CRC region mismatch: requested {region} "
                                  f"({region_id}), reported {actual_region['current']} "
                                  f"with maximum {actual_region['maximum']}")
            low = vcp_get(client, 0xf7)
            high = vcp_get(client, 0xf8)
            if low["maximum"] != 65535 or high["maximum"] != 65535:
                raise TesterError("Firmware CRC returned an invalid result width")
            crc = low["current"] | (high["current"] << 16)
            return {"operation": "firmware-crc", "scope": region,
                    "region_id": region_id, "address": address, "size": size, "pages": pages,
                    "crc32": f"{crc:08x}", "tail_verified": False,
                    "full_image_verified": False,
                    "created_utc": timestamp()}
        time.sleep(0.1)
    raise TesterError(f"Firmware CRC timed out after {timeout} seconds; job not restarted")


def firmware_crc(client, image, timeout=120, region="bank0"):
    """Compare the selected live flash region with bytes at its image offset."""
    if region not in CRC_REGIONS:
        raise TesterError("Unknown firmware CRC region; use bank0 or vendor-probe")
    if region == "bank0":
        if len(image) not in (BANK_SIZE, FULL_IMAGE_SIZE):
            raise TesterError("Firmware CRC requires a 65536-byte bank or 524288-byte image")
    elif len(image) != FULL_IMAGE_SIZE:
        raise TesterError("Vendor-probe CRC requires a complete 524288-byte image")
    _, address, size = CRC_REGIONS[region]
    expected_bytes = image[address:address + size]
    expected = zlib.crc32(expected_bytes)
    result = read_firmware_crc(client, timeout, region=region)
    if int(result["crc32"], 16) != expected:
        raise TesterError(f"Firmware {region} CRC mismatch: expected {expected:08x}, "
                          f"observed {result['crc32']}")
    result.update({"expected_crc32": f"{expected:08x}", "verified": True,
                   "region_sha256": sha256(expected_bytes)})
    if region == "bank0":
        result["bank0_sha256"] = result["region_sha256"]
    return result


def capture(output, device):
    require_new(output)
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise TesterError("ffmpeg is not installed or is not on PATH")
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-n", "-f",
                    "dshow", "-vcodec", "mjpeg", "-video_size", "1920x1080",
                    "-framerate", "30", "-i", "video=" + device, "-an", "-ss", "1",
                    "-frames:v", "1", str(output)], check=True, timeout=30)
    data = output.read_bytes()
    metadata = {"operation": "capture", "device": device, "file": output.name,
                "size": len(data), "sha256": sha256(data), "created_utc": timestamp()}
    write_json_new(metadata_path(output), metadata)
    return metadata


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="COM port; otherwise discover the Feather by USB ID")
    parser.add_argument("--serial", default=DEFAULT_SERIAL, help="Feather USB serial number")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("info", help="Firmware version and current video mode")
    sub.add_parser("scan", help="Scan the HDMI DDC bus")
    sub.add_parser("ddc-config", help="Read fixed DDC registers in ISP; preserve existing ISP")
    get_vcp = sub.add_parser("vcp-get", help="Get a live DDC/CI value through the HSTX tester")
    get_vcp.add_argument("code", type=lambda value: int(value, 0), help="Decimal or 0x-prefixed VCP code")
    set_vcp = sub.add_parser("vcp-set", help="Send a live DDC/CI Set VCP command")
    set_vcp.add_argument("code", type=lambda value: int(value, 0))
    set_vcp.add_argument("value", type=lambda value: int(value, 0))
    key = sub.add_parser("key", help="Send an Adafruit RTD virtual menu key (VCP 0xe0)")
    key.add_argument("name", choices=VIRTUAL_KEYS)
    sub.add_parser("menu-state", help="Read Adafruit RTD menu state (VCP 0xe1)")
    edid = sub.add_parser("edid", help="Read base EDID and all advertised extension blocks")
    edid.add_argument("output", type=Path, help="New binary EDID file")
    dump = sub.add_parser("dump", help="Read flash at least twice and save only matching data")
    dump.add_argument("output", type=Path)
    dump.add_argument("--reads", type=int, default=2)
    verify = sub.add_parser("verify", help="Compare every flash byte with an image")
    verify.add_argument("image", type=Path)
    crc = sub.add_parser("firmware-crc", help="Verify a selected live flash region CRC without ISP")
    crc.add_argument("image", type=Path, help="Full 512 KiB image, or 64 KiB image for bank0 only")
    crc.add_argument("--region", choices=CRC_REGIONS, default="bank0",
                     help="bank0 (default) or retained vendor bytes 0x010000..0x011FFF")
    program = sub.add_parser("program", help="Program changed sectors and verify every byte")
    program.add_argument("image", type=Path)
    program.add_argument("--backup", type=Path, required=True,
                         help="Verified full original/current image, checked before writes")
    program.add_argument("--allow-write", action="store_true", required=True,
                         help="Explicitly authorize flash erase/program operations")
    program.add_argument("--recover", action="store_true",
                         help="Restore a partial failed image; image must equal --backup")
    program.add_argument("--fast", action="store_true",
                         help="Bank0-only update with live CRC; unchanged verified backup tail required")
    program.add_argument("--receipt", type=Path, help="New JSON receipt (default: timestamped)")
    protection = sub.add_parser("restore-protection",
                                help="Verify a complete image, then recover recorded BP protection")
    protection.add_argument("image", type=Path, help="Exact current 512 KiB flash image")
    protection.add_argument("--status", type=lambda value: int(value, 0), required=True,
                            help="Recorded status byte, e.g. 0x0c; only BP bits may change")
    protection.add_argument("--allow-write", action="store_true", required=True)
    protection.add_argument("--receipt", type=Path, required=True, help="New success/failure JSON receipt")
    pattern = sub.add_parser("pattern", help="Select the video test pattern")
    pattern.add_argument("name", choices=PATTERNS)
    mode = sub.add_parser("mode", help="Change video mode and reboot the Feather")
    mode.add_argument("name", choices=("640", "800", "panel", "off"))
    sub.add_parser("reset", help="Reset the RTD controller; use only with a valid flash image")
    sub.add_parser("reset-chip", help="Request RTD whole-chip SOF_RST; valid flash required")
    camera = sub.add_parser("capture", help="Capture one webcam PNG using ffmpeg DirectShow")
    camera.add_argument("output", type=Path)
    camera.add_argument("--device", default="USB Camera")
    return parser


def main(argv=None):
    args = argument_parser().parse_args(argv)
    try:
        if args.action == "capture":
            result = capture(args.output, args.device)
        elif args.action == "restore-protection":
            result = restore_protection(args.port, args.serial, args.image,
                                        args.status, args.receipt)
        else:
            # Refuse existing paths before even opening USB.
            if args.action in ("dump", "edid"):
                require_new(args.output)
            if args.action == "program":
                target = read_image(args.image)
                backup = read_image(args.backup)
                receipt = args.receipt or args.image.with_name(
                    args.image.name + ".program-" + datetime.datetime.now(
                        datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + ".json")
                require_new(receipt, include_metadata=False)
            with Client(args.port, args.serial) as client:
                hello = client.hello()
                if args.action == "info":
                    result = hello
                elif args.action == "scan":
                    result = client.command("scan")
                    addresses = result.get("addresses")
                    if (not isinstance(addresses, list)
                            or any(type(address) is not int or not 0 <= address < 128
                                   for address in addresses)):
                        raise TesterError("Invalid I2C scan response")
                elif args.action == "ddc-config":
                    result = ddc_config(client)
                elif args.action in ("vcp-get", "vcp-set", "key", "menu-state"):
                    if hello.get("isp_active") is not False:
                        raise TesterError("Live DDC/CI requires confirmed ISP inactive; "
                                          "no ISP exit or reset attempted")
                    if args.action == "vcp-get":
                        result = vcp_get(client, args.code)
                    elif args.action == "vcp-set":
                        result = vcp_set(client, args.code, args.value)
                    elif args.action == "key":
                        result = vcp_set(client, 0xe0, VIRTUAL_KEYS[args.name])
                        result.update({"operation": "key", "key": args.name})
                    else:
                        result = vcp_get(client, 0xe1)
                        result["operation"] = "menu-state"
                elif args.action == "edid":
                    blocks = []
                    count = 1
                    while len(blocks) < count:
                        number = len(blocks)
                        response = client.command(f"edid {number}")
                        if response.get("block") != number:
                            raise TesterError("Wrong EDID block in response")
                        blocks.append(hex_data(response, 128))
                        count = 1 + blocks[0][126]
                        if count > 8:
                            raise TesterError("EDID advertises more than the eight blocks "
                                              "supported by this firmware")
                    result = {"operation": "edid", **decode_edid(blocks)}
                    save_binary(args.output, b"".join(blocks), result)
                    if not result["header_valid"] or not all(result["checksums_valid"]):
                        print("Warning: EDID saved, but header/checksum is invalid",
                              file=sys.stderr)
                elif args.action == "dump":
                    result = dump_flash(client, args.output, args.reads)
                elif args.action == "verify":
                    result = verify_flash(client, args.image)
                elif args.action == "firmware-crc":
                    if hello.get("isp_active") is not False:
                        raise TesterError("Live firmware CRC requires confirmed ISP inactive")
                    result = firmware_crc(client, read_image(args.image), region=args.region)
                elif args.action == "program":
                    try:
                        result = program_flash(client, target, backup,
                                               recover=args.recover, fast=args.fast)
                    except Exception as error:
                        write_json_new(receipt, {
                            "operation": "program", "recovery": args.recover, "fast": args.fast,
                            "verified": False,
                            "error": str(error), "backup": str(args.backup),
                            "backup_sha256": sha256(backup), "target": str(args.image),
                            "target_sha256": sha256(target), "created_utc": timestamp()})
                        raise
                    result.update({"backup": str(args.backup), "target": str(args.image),
                                   "port": client.port})
                    write_json_new(receipt, result)
                    result["receipt"] = str(receipt)
                elif args.action in ("pattern", "mode"):
                    result = client.command(args.action + " " + args.name)
                else:
                    result = client.command(args.action, timeout=15)
        print(json.dumps(result, indent=2))
        return 0
    except (TesterError, OSError, ValueError, serial.SerialException,
            subprocess.SubprocessError) as error:
        print("Error: " + str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted. If a write was active, keep the display powered and "
              "recover its verified image before finish/reset.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
