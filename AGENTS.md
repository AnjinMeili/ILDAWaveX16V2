# Notes for coding agents

Two firmwares drive one laser DAC board. The ESP32-S3 (`firmware/ESP32S3`, ESP-IDF) handles the network, SD card and web UI. It sends 512-sample packets over SPI to the RP2354 (`firmware/RP2354`, Pico SDK), which owns the output buffer and the DAC timing. The RP2354 also takes UART commands from the ESP32-S3: `STATUS`, `RATE`, `BUFFER`, `START` and `STOP`.

## Safety

A laser may be attached and live.

- Never start output (Play, Project, test patterns, protocol senders) unless the user asks.
- Before flashing either chip, confirm output is stopped: `remote_running` is false in `GET /api/rp/status`. Flashing mid-stream drops the stream; the RP blanks within about 20 ms.
- Ask before pushing. `origin` is the upstream project; push only to the user's fork (`fork`).

## Hardware access

- One USB-C port serves both chips; which one you get depends on cable orientation. Check before flashing with `ioreg -p IOUSB -l -w0 | grep '"USB Product Name"'`: `USB JTAG_serial debug unit` is the ESP32-S3, `Pico` is the RP2354. Both enumerate as `/dev/cu.usbmodem101`.
- Opening the ESP32-S3 port with pyserial's defaults resets the chip. Log with `tools/serial_logger.py`, which does not reset it, and stop the logger before flashing.
- `tools/stream_stats.py` prints points/s per protocol, underruns/s and RP fill from the status API. Use it to measure throughput instead of judging by eye.
- Logs go in `logs/`, which is gitignored.

## Build and flash

- **ESP32-S3:** `export IDF_PATH=$HOME/.espressif/v6.1/esp-idf && . $IDF_PATH/export.sh`, then from `firmware/ESP32S3` run `idf.py build` and `idf.py -p /dev/cu.usbmodem101 flash`.
- `sdkconfig` is gitignored and wins over `sdkconfig.defaults` once it exists. When changing a Kconfig value, edit `sdkconfig.defaults` (committed) and the matching line in `sdkconfig`, then confirm the value in `build/config/sdkconfig.h`.
- **RP2354:** run `~/.pico-sdk/ninja/v1.13.2/ninja -C build` from `firmware/RP2354`. Flash with `~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool load -f -x build/ILDAWaveX16V2-RP2350.uf2`, which reboots the RP into BOOTSEL by itself with no buttons.
- Build after every edit. Don't leave a file that doesn't compile.

## Constraints learned the hard way

- **W5500 receive buffer:** the W5500 holds incoming frames in a 16 KB buffer until the ESP32 reads them over SPI. A TCP window larger than that lets a sender burst past it; frames drop and TCP stalls on retransmits. The window is set to 11,520 bytes for this reason.
- **The RP's `BUFFER` command writes RP flash** and stops output. Don't call it per stream; pace from the ESP32 side instead (see `ED_RP_CUSHION` in `etherdream_protocol.c`).
- **UART status reads block** for a variable time (up to 200 ms timeout). Never make one in a real-time feed loop; read in a separate task and extrapolate between readings, as `rp_poll_task` does.
- **DAC timing:** the divider in `dac80508.c` must match the cycles per word in `dac80508_tx.pio` (`DAC_PIO_CYCLES_PER_WORD`). If you change the PIO program, update it. The RP log's `dma_words_to_pio_fifo` divided by words per sample (active channels + 1) gives the real output rate.
- **Flow control differs by protocol.** Ether Dream clients pace from `buffer_fullness`, so it must count everything not yet played. IDN and ILW have no flow control; their RP buffer level is set by the start prebuffer.
- **Diagnostics flags:** `ED_DIAG` in `etherdream_protocol.c` logs per-second client pacing: commands, points, reported fullness, wait and receive time.

## Conventions

- Commit subjects start with the firmware: `ESP32S3: ...` or `RP2354: ...`. Use one commit per fix, with the measured before and after in the body.
- Commit after the user has confirmed the change on hardware.
- `BRINGUP.md` (local, untracked) is the hardware bring-up log. Add dated entries there for what was verified on the board.
