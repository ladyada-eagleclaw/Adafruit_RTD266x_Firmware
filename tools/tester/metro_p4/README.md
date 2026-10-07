# Metro P4 RTD programmer

The Metro programs an RTD2660 through the LT8912B adapter's DDC switch.
It holds the LT8912B in reset on GPIO20 to isolate its conflicting I2C
address 0x4A. GPIO33/32 carry DDC at 100 kHz; GPIO19 stays an input.
The display needs its own power. No video runs in this sketch.

1. Upload `Metro_P4_RTD_Programmer` with Arduino ESP32 3.3.11,
   `esp32:esp32:adafruit_metro_esp32p4`, USB Serial/JTAG, pre-v3 silicon.
2. Turn the adapter's DDC switch **ON**. A QT cable is unnecessary.
3. Use the existing host CLI with an explicit Metro port:

   ```sh
   python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py --port COM39 info
   python tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py --port COM39 dump current-full.bin
   ```

4. Follow the existing [backup and programming procedure](../README.md).
   The shared ISP driver and full-image verification are unchanged.
5. Turn DDC **OFF** before uploading or starting Metro video firmware.
   Remove any QT cable joining the Metro and adapter DDC buses as well.

The sketch refuses register/ISP commands unless GPIO20 remains LOW and both
LT8912B control addresses 0x48/0x49 return address NACK. Bus timeouts do not
count as isolation. Only `mode off` is supported; it does not reboot.
`scan` can inspect addresses without entering ISP. The host opens ESP native
USB Serial/JTAG with DTR/RTS deasserted to avoid resetting the programmer.
The sketch enlarges the USB receive buffer to 2048 bytes before opening serial;
the default 256-byte buffer cannot hold a complete flash-page command.

The C/header wrappers reuse the maintained Feather ISP implementation.
The serial protocol identity remains `FeatherDVI-RTD` for host compatibility;
the `board` field identifies `Metro P4`.
