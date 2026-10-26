# ILDAWaveX16 V2 — Dual-MCU USB-C Splitter

Build notes for a splitter that connects both MCUs on the ILDAWaveX16 V2 (ESP32-S3 + RP2354) to two separate USB host ports at the same time, instead of flipping the cable to switch between them.

## How the board's "cable flip" works

Both MCUs share one USB-C receptacle. Unlike a normal USB-C device, the board does **not** tie the receptacle's two data pairs together:

- **A6/A7** (D+/D−) → ESP32-S3 (`ESP_USB_D+/D−`)
- **B6/B7** (D+/D−) → RP2354 (`RP_USB_D+/D−`)

This is from the J1 connector in `hardware/ILDAWaveX16_V2_REV_00_SCH.pdf`.

A standard USB-C cable has only **one** D+/D− pair. Its plug has contacts only at A6/A7. Flipping the plug decides which receptacle pair that single pair touches, and that picks the MCU.

**Key fact:** no off-the-shelf cable carries both pairs, including USB4 and "full-featured" cables. The splitter needs a plug that has contacts on **both** A6/A7 and B6/B7.

## Parts

- **USB-C male plug breakout, 24-pin**, with separate pads for A6, A7, B6, and B7. Many "16-pin" plug breakouts omit B6/B7, so verify with a meter.
- **Two sacrificial USB-A cables**, cut open. USB-A on the host end avoids CC negotiation.
- Optional: 2× Schottky diodes for a VBUS diode-OR, plus heatshrink or a printed shell for strain relief.

## Wiring

| Breakout pin           | Cable 1 | Cable 2                     |
|------------------------|---------|-----------------------------|
| A6 (D+)                | green   | —                           |
| A7 (D−)                | white   | —                           |
| B6 (D+)                | —       | green                       |
| B7 (D−)                | —       | white                       |
| GND (A1/A12/B1/B12)    | black   | black                       |
| VBUS (A4/A9/B4/B9)     | red     | **leave cut / insulated**   |
| CC1 / CC2              | leave unconnected | leave unconnected |

### Why each choice matters

- **VBUS from one host only.** Tying both hosts' 5 V rails together back-feeds them. Power the board from cable 1, or put a Schottky diode in each red wire to diode-OR them.
- **Cable 2 carries only data and ground.** The whole board, both chips included, runs from cable 1's VBUS. All four VBUS pins join on the board, so the insert direction does not change this. The port cable 1 plugs into must supply the whole board. Measured on 2026-10-26 with an inline USB meter on cable 1, the board drew 0.252 A at 5 V (about 1.3 W), idle with output stopped. Current while output is running has not been measured yet. The board's VBUS fuse (F1) is 0.5 A, so allow for up to 500 mA. The idle board ran fine from an unpowered hub (hub plus board drew 0.335 A from the computer).
- **A powered hub on cable 1** takes the load off the computer. Its own supply then feeds the board. Cable 2 can stay on the computer, or go to the same hub. Ground stays common through the hub's upstream cable.
- **CC can float with USB-A hosts.** A USB-A port supplies 5 V unconditionally. A USB-C host port only enables VBUS after it sees a 5.1 kΩ Rd on CC. If you use C-to-C host cables, add a 5.1 kΩ resistor from each **host-side** CC to GND in the adapter. Still don't connect the board's CC pins.
- **Common ground.** Both cable grounds tie together. That's fine on the same PC or a shared hub.
- **Signal integrity is forgiving.** Both ESP32-S3 and RP2354 native USB run at Full Speed (12 Mbps). Keep each D+/D− pair twisted, with untwisted pigtails under about 2–3 cm. Twisting was enough for the 2026-10-26 tests: both chips enumerated and flashed. If you see dropouts or failed enumeration, clip a snap-on ferrite around the whole cable near the splitter.

## Identifying which pair goes to which MCU

1. Plug in cable 1 only and see what enumerates:
   - ESP32-S3: `USB JTAG_serial debug unit` (vendor:product 303A:1001)
   - RP2354: `Pico` (vendor 2E8A) when running the firmware, or a UF2 mass-storage drive when in BOOTSEL mode
2. Label the cables.
3. Mark the plug shell (dot or key) so orientation stays consistent, since the plug can still be inserted either way. Cable 2 alone shows nothing, because it carries no power.
4. Optionally confirm against the schematic in the repo's `hardware/` folder.

### Checking both chips on macOS

```sh
ioreg -p IOUSB -l -w0 | grep '"USB Product Name"'; ls /dev/cu.usbmodem*
```

With the splitter working, both `USB JTAG_serial debug unit` and `Pico` appear, and there are two `/dev/cu.usbmodem*` ports. If only one name appears, check the other pair's D+/D− order and its ground.

## USB-C male plug pinout

Viewed **looking into the plug's open end**. This is mirrored compared to a receptacle: on a plug, A1 is on the right.

### Normal orientation

| Row A | A12 | A11  | A10  | A9   | A8   | **A7** | **A6** | A5  | A4   | A3   | A2   | A1  |
|-------|-----|------|------|------|------|--------|--------|-----|------|------|------|-----|
| Signal| GND | RX2+ | RX2− | VBUS | SBU1 | **D−** | **D+** | CC1 | VBUS | TX1− | TX1+ | GND |

*(cavity, where the receptacle tongue slides in)*

| Row B | B1  | B2   | B3   | B4   | B5  | **B6** | **B7** | B8   | B9   | B10  | B11  | B12 |
|-------|-----|------|------|------|-----|--------|--------|------|------|------|------|-----|
| Signal| GND | TX2+ | TX2− | VBUS | CC2 | **D+** | **D−** | SBU2 | VBUS | RX1− | RX1+ | GND |

### Rotated 180° (flipped cable)

| Row B | B12 | B11  | B10  | B9   | B8   | **B7** | **B6** | B5  | B4   | B3   | B2   | B1  |
|-------|-----|------|------|------|------|--------|--------|-----|------|------|------|-----|
| Signal| GND | RX1+ | RX1− | VBUS | SBU2 | **D−** | **D+** | CC2 | VBUS | TX2− | TX2+ | GND |

*(cavity)*

| Row A | A1  | A2   | A3   | A4   | A5  | **A6** | **A7** | A8   | A9   | A10  | A11  | A12 |
|-------|-----|------|------|------|-----|--------|--------|------|------|------|------|-----|
| Signal| GND | TX1+ | TX1− | VBUS | CC1 | **D+** | **D−** | SBU1 | VBUS | RX2− | RX2+ | GND |

Flipping the plug puts the B6/B7 pair where A6/A7 was. That swap is the whole mechanism that selects the other MCU.

### Pinout notes

- Data lines sit in the **center four columns**, the easiest pins to tap.
- **B6/B7 are often absent on ordinary cable plugs.** That's why a normal cable reaches only one MCU.
- **B5 is CC2** on a breakout plug. In e-marked cables it carries VCONN. Irrelevant here, since CC stays unconnected.
- If your breakout has silkscreen labels, go by those rather than by position.

## Breakout checks before building

- **Plug breakout:** verify continuity on B6 and B7 from pad to contact.
- **Female (receptacle) breakouts:** many tie A6↔B6 and A7↔B7 on the PCB so either orientation works. That would put both MCUs on the same pair. With nothing plugged in, beep A6 to B6. It must be **open**.
- **Male-to-female 24-pin passthrough adapters:** some pass B6/B7 straight through, many don't. Test by plugging the adapter into a female breakout and checking B6/B7 continuity from the male end.

## Packaging

- Simplest build: plug breakout, two cables, heatshrink, and potting or a printed shell. The plug breakout takes all the strain otherwise.
- Neater build: a tiny PCB with a vertical-mount USB-C plug feeding two USB-A plugs or two receptacles, plus a jumper or pad for the VBUS source and diode option.

## Result

With the splitter in place, you can flash the RP2354 (BOOTSEL / UF2) and watch the ESP32-S3 console at the same time, with no cable flipping.

Tested 2026-10-26: the RP2354 and then the ESP32-S3 were flashed one after the other, and each chip stayed on USB while the other was flashed. Edit, build, flash and test for both firmwares becomes one loop with no hands on the board, which makes quick iteration practical.

## Sources

- [ILDAWaveX16V2 GitHub README](https://github.com/stanleyondrus/ILDAWaveX16V2): both MCUs share one USB-C connector, and orientation selects the MCU.
- [ILDAWaveX16 V2 on Tindie](https://www.tindie.com/products/stanleyprojects/ildawavex16-v2/)
- [Project documentation](https://stanleyprojects.com/projects/ildawavex16v2)
