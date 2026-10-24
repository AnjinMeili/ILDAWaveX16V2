# Flashing RP2354

Use a released `.uf2` file or build one with the [RP Docker builder](building-rp2354.md). Local builds export `dist/rp2354/artifacts/ILDAWaveX16V2-RP2350.uf2`.

## Enter the bootloader

1. Connect the computer to the board's RP USB connection with a data-capable cable.
2. Hold `RP BOOT`.
3. Briefly press and release `RP RST` while holding `RP BOOT`.
4. Release `RP BOOT`. The RP should appear as a USB storage drive.

These are the RP buttons; the ESP has separate boot and reset buttons. This procedure updates only the RP firmware.

## Browser flashing

The third-party [pico⚡flash](https://picoflash.org/) tool supports RP2350 devices through WebUSB. Use Chrome or Edge; see its [browser/platform requirements](https://github.com/piersfinlayson/picoflash). This is separate from the ESP32 web installer.

1. Enter the RP bootloader as described above.
2. Open [pico⚡flash](https://picoflash.org/), click **Connect**, and select the RP bootloader device.
3. In the **Flash** tab, select or drop the `.uf2` file.
4. Click **Flash Device** and keep USB connected until it reports success.
5. If the board remains in the bootloader, click **Reboot** or briefly press `RP RST` with `RP BOOT` released.

Use the UF2 file; no manual address, raw binary conversion, advanced erase, or OTP operation is needed. Settings preservation has not been validated.

Browser flashing of this board and the generated UF2 remains untested. If connection fails, close other flashing tools or browser tabs, check USB permission and the data cable, and re-enter the bootloader. The mounted-drive method below is the fallback, including on Windows if WebUSB cannot claim the device.

The generated UF2 contains an RP2350-E10 compatibility block outside the normal application flash range. If the browser reports an address or family error, retain the error log and use the original UF2 with the mounted-drive method. Do not remove blocks or switch to a raw binary to bypass the error.

## Mounted-drive flashing

Enter the RP bootloader, then copy the `.uf2` onto its USB storage drive using your file manager. The drive should disappear and the board should reboot when programming completes.

After either method, refresh the ESP web interface and check the RP connection and reported firmware version. Rebuilding the same version does not produce a distinct identifier, so an unchanged version alone cannot confirm which build is running. Keep the previous release UF2 if you need to restore it.
