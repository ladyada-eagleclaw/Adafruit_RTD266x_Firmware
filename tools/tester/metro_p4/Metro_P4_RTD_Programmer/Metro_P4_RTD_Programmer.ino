// SPDX-FileCopyrightText: 2026 Limor Fried for Adafruit Industries
// SPDX-License-Identifier: MIT
// Adafruit Metro P4 RTD programmer, compatible with the existing host.py.
// Reuses the Feather tester protocol and the shared checked ISP driver.
// Connect the adapter DDC switch to the powered RTD display for programming.
// GPIO20 holds LT8912B reset LOW throughout; GPIO19 remains input-only.
// No DSI, framebuffer, audio, or APLL is initialized by this sketch.

#include <Arduino.h>
#include <Wire.h>
#include <errno.h>
#include "RTD266xISP.h"

const uint16_t BRIDGE_RESET_PIN = 20;
const uint16_t BRIDGE_HPD_PIN = 19;
const uint16_t SERIAL_LINE_BYTES = 1024;
const uint8_t DDC_CI_ADDRESS = 0x37;
const uint8_t DDC_CI_MAX_BYTES = 32;
RTD266xISP flash;
char commandLine[SERIAL_LINE_BYTES];
uint16_t commandLength = 0;
bool lineOverflow = false;
bool i2cReady = false;

void setup() {
  // Set the output latch before enabling the output. Never release reset.
  digitalWrite(BRIDGE_RESET_PIN, LOW);
  pinMode(BRIDGE_RESET_PIN, OUTPUT);
  pinMode(BRIDGE_HPD_PIN, INPUT);
  // A 256-byte flash page is over 512 serial characters in this protocol.
  // Native USB Serial/JTAG's default 256-byte RX queue would truncate it.
  Serial.setRxBufferSize(SERIAL_LINE_BYTES * 2);
  Serial.begin(115200);
  delay(250);

  i2cReady = Wire.begin(33, 32, 100000);
  Wire.setTimeOut(100);
  // Keep the protocol identity expected by host.py; hello identifies the board.
  Serial.println("{\"event\":\"ready\",\"firmware\":\"FeatherDVI-RTD\",\"board\":\"Metro P4\"}");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      commandLine[commandLength] = 0;
      if (lineOverflow) {
        printError("Command is too long");
      } else if (commandLength) {
        processCommand(commandLine);
      }
      commandLength = 0;
      lineOverflow = false;
    } else if (commandLength < SERIAL_LINE_BYTES - 1) {
      commandLine[commandLength++] = c;
    } else {
      lineOverflow = true;
    }
  }
  delay(1);
}

bool bridgeIsolated() {
  if (!i2cReady || digitalRead(BRIDGE_RESET_PIN) != LOW) {
    printError("I2C is unavailable or LT8912B reset is not LOW");
    return false;
  }
  for (uint8_t address = 0x48; address <= 0x49; ++address) {
    Wire.beginTransmission(address);
    uint8_t status = Wire.endTransmission();
    if (status == 0) {
      printError("LT8912B still responds; refusing RTD ISP address collision");
      return false;
    }
    // Require a definite address NACK, not a timeout or another bus error.
    if (status != 2) {
      printError("Cannot verify LT8912B isolation: I2C bus error");
      return false;
    }
  }
  return true;
}

void printError(const char *message) {
  Serial.print("{\"ok\":false,\"error\":\"");
  Serial.print(message);
  Serial.println("\"}");
}

void printResult(bool ok) {
  if (ok) {
    Serial.println("{\"ok\":true}");
  } else {
    printError(flash.error());
  }
}

void printHex(const uint8_t *data, size_t length) {
  const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < length; i++) {
    Serial.write(hex[data[i] / 16]);
    Serial.write(hex[data[i] % 16]);
  }
}

bool number(const char *text, uint32_t &value) {
  if (!text || !*text || *text == '-') {
    return false;
  }
  char *end;
  errno = 0;
  unsigned long parsed = strtoul(text, &end, 0);
  if (errno || *end) {
    return false;
  }
  value = parsed;
  return true;
}

bool hexBytes(const char *text, uint8_t *data, size_t &length) {
  if (!text) {
    return false;
  }
  size_t chars = strlen(text);
  if (!chars || chars % 2 || chars > 512) {
    return false;
  }
  length = chars / 2;
  for (size_t i = 0; i < length; i++) {
    char pair[3] = {text[2 * i], text[2 * i + 1], 0};
    if (!isxdigit(pair[0]) || !isxdigit(pair[1])) {
      return false;
    }
    data[i] = strtoul(pair, nullptr, 16);
  }
  return true;
}

bool readEdid(uint8_t block, uint8_t *data) {
  bool ok = true;
  if (block >= 2) {
    Wire.beginTransmission(0x30);
    Wire.write(block / 2);
    ok = Wire.endTransmission() == 0;
  }
  for (uint16_t offset = 0; ok && offset < 128; offset += 32) {
    Wire.beginTransmission(0x50);
    Wire.write((uint8_t)((block % 2) * 128 + offset));
    if (Wire.endTransmission(false)) {
      ok = false;
      break;
    }
    if (Wire.requestFrom((uint8_t)0x50, (uint8_t)32) != 32) {
      ok = false;
      break;
    }
    for (uint8_t i = 0; i < 32; i++) {
      data[offset + i] = Wire.read();
    }
  }
  // Also restore the segment pointer after a short read or lost ACK.
  if (block >= 2) {
    Wire.beginTransmission(0x30);
    Wire.write((uint8_t)0);
    if (Wire.endTransmission()) {
      ok = false;
    }
  }
  return ok;
}

void ddcTransfer(const char *packet, const char *replyLength) {
  uint8_t data[256]; // hexBytes() also serves the 256-byte flash page command.
  size_t bytes = 0;
  uint32_t length = 0;
  if (!packet || !number(replyLength, length) || length > DDC_CI_MAX_BYTES) {
    printError("DDC requires a hex packet and 0, or - and a read length 1..32");
    return;
  }
  bool reading = !strcmp(packet, "-");
  if ((reading && !length) ||
      (!reading && (length || !hexBytes(packet, data, bytes) ||
                    bytes > DDC_CI_MAX_BYTES))) {
    printError("DDC uses separate writes and reads, each at most 32 bytes");
    return;
  }
  if (flash.active()) {
    printError("Live DDC requires the RTD firmware running, outside ISP");
    return;
  }
  // Keep the 50 ms DDC/CI processing delay on the host. Each command performs
  // one short bus transaction.
  Wire.setTimeOut(10);
  bool ok;
  if (reading) {
    bytes = Wire.requestFrom(DDC_CI_ADDRESS, (uint8_t)length);
    ok = bytes == length;
    for (size_t i = 0; i < bytes; ++i) {
      data[i] = Wire.read();
    }
  } else {
    Wire.beginTransmission(DDC_CI_ADDRESS);
    ok = Wire.write(data, bytes) == bytes;
    if (ok) {
      ok = Wire.endTransmission() == 0;
    }
  }
  Wire.setTimeOut(100); // Restore the existing timeout for EDID and ISP.
  if (!ok) {
    printError("DDC transaction failed or returned a short read; not retried");
  } else if (reading) {
    Serial.print("{\"ok\":true,\"data\":\"");
    printHex(data, bytes);
    Serial.println("\"}");
  } else {
    Serial.print("{\"ok\":true,\"written\":");
    Serial.print(bytes);
    Serial.println("}");
  }
}

void processCommand(char *line) {
  char *cmd = strtok(line, " ");
  if (!cmd) {
    return;
  }
  char *arg1 = strtok(nullptr, " ");
  char *arg2 = strtok(nullptr, " ");
  if (strtok(nullptr, " ")) {
    printError("Too many arguments");
    return;
  }
  // Every register transaction requires the bridge held in reset. Its AVI
  // address collides with the RTD ISP address, including ordinary reads.
  if (strcmp(cmd, "scan") && !bridgeIsolated()) {
    return;
  }
  uint32_t address = 0;
  uint32_t length = 0;
  uint8_t data[256];
  if (!strcmp(cmd, "hello")) {
    Serial.print("{\"ok\":true,\"firmware\":\"FeatherDVI-RTD\",\"protocol\":1,\"video\":\"");
    Serial.print("off\",\"framebuffer\":\"none\",\"panel_timing\":false");
    Serial.print(",\"board\":\"Metro P4\"");
    Serial.print(",\"isp_active\":");
    // Recover the actual RTD state even if the programmer itself has rebooted.
    // An absent or unresponsive RTD is unknown, never a false running claim.
    bool ispActive;
    if (flash.readISPState(ispActive)) {
      Serial.print(ispActive ? "true" : "false");
    } else {
      Serial.print("null");
    }
    Serial.println("}");
  } else if (!strcmp(cmd, "scan")) {
    Serial.print("{\"ok\":true,\"addresses\":[");
    bool first = true;
    for (uint8_t i = 1; i < 127; i++) {
      Wire.beginTransmission(i);
      if (Wire.endTransmission() == 0) {
        if (!first) {
          Serial.print(',');
        }
        Serial.print(i);
        first = false;
      }
    }
    Serial.println("]}");
  } else if (!strcmp(cmd, "edid")) {
    if ((arg1 && !number(arg1, address)) || address > 7) {
      printError("EDID block must be 0 through 7");
    } else if (!readEdid(address, data)) {
      printError("EDID read failed");
    } else {
      Serial.print("{\"ok\":true,\"block\":");
      Serial.print(address);
      Serial.print(",\"data\":\"");
      printHex(data, 128);
      Serial.println("\"}");
    }
  } else if (!strcmp(cmd, "ddc")) {
    ddcTransfer(arg1, arg2);
  } else if (!strcmp(cmd, "ddc-config")) {
    RTD266xISP::DDCConfig config;
    if (arg1 || arg2) {
      printError("DDC configuration takes no arguments");
    } else if (!flash.readDDCConfig(config)) {
      printError(flash.error());
    } else {
      Serial.print("{\"ok\":true,\"registers\":{");
      for (size_t i = 0; i < RTD266xISP::DDC_REGISTER_COUNT; i++) {
        if (i) {
          Serial.print(',');
        }
        Serial.print('"');
        Serial.print(RTD266xISP::DDC_REGISTERS[i].name);
        Serial.print("\":\"");
        printHex(&config.values[i], 1);
        Serial.print('"');
      }
      Serial.print("},\"channel_access\":{\"FFEC\":\"");
      printHex(&config.channelAccess[0], 1);
      Serial.print("\",\"FFED\":\"");
      printHex(&config.channelAccess[1], 1);
      Serial.println("\"}}");
    }
  } else if (!strcmp(cmd, "isp")) {
    uint8_t status;
    if (!flash.enter() || !flash.readStatus(status)) {
      printError(flash.error());
    } else {
      Serial.print("{\"ok\":true,\"jedec\":\"");
      Serial.print(flash.jedecId(), HEX);
      Serial.print("\",\"size\":");
      Serial.print(flash.flashSize());
      Serial.print(",\"status\":");
      Serial.print(status);
      Serial.println("}");
    }
  } else if (!strcmp(cmd, "read")) {
    if (!number(arg1, address) || !number(arg2, length) || !length || length > 256) {
      printError("Read requires an address and length 1 through 256");
    } else if (!flash.read(address, data, length)) {
      printError(flash.error());
    } else {
      Serial.print("{\"ok\":true,\"address\":");
      Serial.print(address);
      Serial.print(",\"data\":\"");
      printHex(data, length);
      Serial.println("\"}");
    }
  } else if (!strcmp(cmd, "arm")) {
    if (!arg1 || strlen(arg1) != 6 || strspn(arg1, "0123456789abcdefABCDEF") != 6) {
      printError("Arm requires the six-digit flash JEDEC ID");
    } else {
      printResult(flash.arm(strtoul(arg1, nullptr, 16)));
    }
  } else if (!strcmp(cmd, "unlock")) {
    printResult(flash.unlock());
  } else if (!strcmp(cmd, "restore-protection")) {
    if (!number(arg1, address) || address > 255) {
      printError("restore-protection requires a recorded status byte");
    } else {
      printResult(flash.restoreProtection((uint8_t)address));
    }
  } else if (!strcmp(cmd, "erase")) {
    if (!number(arg1, address)) {
      printError("Erase requires a sector address");
    } else {
      printResult(flash.eraseSector(address));
    }
  } else if (!strcmp(cmd, "page")) {
    size_t bytes = 0;
    if (!number(arg1, address) || !hexBytes(arg2, data, bytes)) {
      printError("Page requires an address and 1 through 256 hex bytes");
    } else {
      printResult(flash.programPage(address, data, bytes));
    }
  } else if (!strcmp(cmd, "finish")) {
    printResult(flash.finish());
  } else if (!strcmp(cmd, "reset")) {
    printResult(flash.reset());
  } else if (!strcmp(cmd, "reset-chip")) {
    printResult(flash.resetChip());
  } else if (!strcmp(cmd, "mode")) {
    if (!arg1 || strcmp(arg1, "off") || arg2) {
      printError("This programmer supports only mode off");
    } else {
      Serial.println("{\"ok\":true,\"rebooting\":false}");
    }
  } else if (!strcmp(cmd, "pattern")) {
    printError("Video is disabled in the Metro P4 programmer");
  } else {
    printError("Unknown command");
  }
}
