# ILDAWaveX16 V2
[![](https://stanleyprojects.com/projects/ildawavex16v2/thumbs/cover-text.jpg)](https://stanleyprojects.com/projects/ildawavex16v2)

**ILDAWaveX16 V2** is an all-in-one, dual-processor, high-resolution laser DAC platform built for makers and laser enthusiasts.

It combines **ILDA DB25 output**, **SD-card playback**, **Ethernet**, **Wi-Fi**, **USB**, **JST XH connectors**, and a flexible firmware architecture designed for experimenting with laser protocols and custom workflows.

The board is built around two processors:

- **ESP32-S3** - networking, web UI, SD-card playback, protocol parsing, configuration
- **RP2354** - real-time buffering, DAC timing, deterministic laser output

For full project documentation, visit: https://stanleyprojects.com/projects/ildawavex16v2

> [!WARNING]
> ILDAWaveX16 is a maker-friendly laser DAC platform, not a finished commercial laser product. The firmware is still under development and may contain bugs, so you should be comfortable flashing firmware, modifying code, and troubleshooting embedded hardware and software issues.

---

## Repository Contents

The firmware is split into separate projects:

```text
ILDAWaveX16/
├── firmware/
│   ├── ESP32S3/      # ESP32-S3 FW: networking, web UI, SD, protocols
│   └── RP2354/       # RP2354 FW: real-time DAC output engine
├── hardware/         # schematic, 3D model
├── tools/            # scripts for protocol testing
└── README.md
```

> [!NOTE]
> The ESP32-S3 firmware is also compatible with the original **ILDAWaveX16 V1** board. To build for V1, set `BOARD_VERSION` to `1` in `ESP32S3/main/CMakeLists.txt`.
> Performance on V1 is currently more limited than on V2, so please treat V1 support as experimental.

---

## Firmware Development Setup

Recommended development tools:

- [Visual Studio Code](https://code.visualstudio.com/)
- [ESP-IDF Extension for VS Code](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension) for the ESP32-S3 firmware
- [Raspberry Pi Pico Extension for VS Code](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico) for the RP2354 firmware

The ESP32-S3 firmware is built with ESP-IDF.

The RP2354 firmware is built with the Raspberry Pi Pico SDK and produces a `.uf2` firmware file.

---

## Firmware Architecture

ILDAWaveX16 V2 uses a split architecture to keep the user-facing and real-time parts of the system separated.

The **ESP32-S3** handles:

- Web interface
- REST API
- Ethernet / Wi-Fi configuration
- microSD card access
- ILDA file playback
- Protocol parsing
- Test-pattern generation
- Stream conversion and packet forwarding to RP2354
- Runtime configuration and diagnostics

The **RP2354** handles:

- Real-time DAC sample output
- SPI packet receiving with PIO
- DMA-based payload handling
- Ring buffer management
- READY hardware flow control
- X/Y mirroring
- Underrun handling
- Safe blanking and hard-zero stop behavior

Incoming data from SD files, test patterns, or network protocols is converted on the ESP32-S3 into a unified internal sample format.

Each laser point is represented as an 8-channel signed 16-bit sample:

```text
X, Y, R, G, B, I, US1, US2
```

---

## Flashing Firmware

Both microcontrollers are connected to the same USB-C connector.

Depending on the USB-C cable orientation, the port connects either to the **ESP32-S3** or to the **RP2354**.

If the expected device does not appear on your computer, unplug the USB-C cable, rotate it 180 degrees, and plug it in again.

Each microcontroller must be flashed separately.

---

### Flashing ESP32-S3

If building from source, use the ESP-IDF extension in VS Code.

Before building, choose the target board version in: `ESP32S3/main/CMakeLists.txt`

If flashing a released firmware image, use the [web flasher](https://stanleyprojects.com/projects/ildawavex16v2/install/index.html).

The ESP32-S3 should normally enter bootloader mode automatically. If flashing does not start:

1. Hold `ESP BOOT`
2. Briefly press and release `ESP RST`
3. Try flashing again

---

### Flashing RP2354

If building from source, use the Raspberry Pi Pico extension in VS Code.

If flashing a released firmware image, enter bootloader mode:

1. Hold `RP BOOT`
2. Briefly press and release `RP RST`

The RP2354 should mount as a storage drive.

Copy the `.uf2` firmware file onto the drive. It will automatically unmount and reboot into the new firmware.

---

## Current Firmware Support

### SD Card Playback

The ESP32-S3 can play `.ild` files directly from microSD.

ILDA frames are parsed, converted into X/Y/color/intensity samples, and streamed to the RP2354 through the same SPI payload path used by live protocols.

The current implementation loads converted frames into PSRAM-backed memory, which makes playback fast and stable, but limits the maximum file size that can be played.

A future improvement would be continuous batch reading, where only partial chunks of the ILDA file are read, converted, and streamed at a time. This would allow larger files to be played without loading the full converted output into memory.

---

### IDN / ILDA Digital Network

The firmware includes an experimental IDN / ILDA Digital Network implementation over UDP.

It supports IDN-Hello discovery, allowing compatible software to find the device on the network and identify it as an IDN-capable laser DAC.

Once connected, incoming IDN real-time stream data is parsed, converted into the internal 8-channel sample format, and forwarded to the RP2354 output engine.

This protocol path works, but still needs optimization at higher point rates.

---

### Ether Dream

The firmware includes an Ether Dream compatible DAC server, allowing standard laser software to discover and stream point data to the board.

Implemented features include:

- UDP discovery
- TCP control
- TCP streaming
- Point-rate configuration
- Prepare / start / stop behavior
- Ether Dream point conversion into the internal 8-channel sample format

This protocol path works, but still needs optimization at higher point rates.

---

### USB Protocol Experiments

Both the ESP32-S3 and RP2354 have native USB, so the board can also be used for USB-based applications.

As a proof of concept, a Helios DAC USB protocol implementation was tested and ran reliably directly on the RP2354. Due to licensing concerns, this implementation is not included in the public firmware. However, the test showed that the hardware is suitable for custom USB protocol development, including USB streaming as an alternative to network streaming.

---

## Hardware Documentation

The schematic is published for reference, repair, education, and firmware development.

PCB manufacturing files, production files, and full hardware source files are not published. The ILDAWaveX16 name, branding, documentation, artwork, and released design files may not be used for commercial manufacturing or resale without permission.

For fully open-source hardware, see the previous ILDAWaveX16 generation with complete KiCad source files:

https://stanleyprojects.com/projects/ildawavex16

---

## Firmware License

The firmware source code is released under the **GPLv3** license.

You may study, modify, and redistribute the firmware under the terms of the GPLv3 license.

Firmware derivatives must remain open under the same license.

---

## Community

Developers are encouraged to study, modify, optimize, and adapt the firmware for new protocols and workflows.

The community has already done a great job helping shape the project, so join the [Discord server](https://discord.gg/B33ecuS3Sw), share your ideas, and build something cool with us.