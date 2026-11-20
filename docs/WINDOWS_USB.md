# Windows: USB driver for firmware flashing

LibreTune talks to ECU bootloaders through **WinUSB**, the generic USB driver built into Windows.
Windows does not bind it automatically to the STM32 or OpenBLT bootloaders, so it has to be set once
per bootloader device (per USB port is not needed). Your ECU's normal serial connection (`COMx`) is
not affected.

## With Zadig (recommended)

1. Put the ECU in its bootloader: in LibreTune, Tools > Firmware Update > *Reboot ECU into DFU*, or
   hold the board's BOOT button while you plug it in.
2. Download Zadig from https://zadig.akeo.ie and run it.
3. Options > List All Devices. Pick the bootloader:
   - `STM32 BOOTLOADER` (USB ID `0483 DF11`) for USB DFU
   - `OpenBLT` (USB ID `1D50 60AC`) for OpenBLT updates
4. Make sure the target driver (right of the arrow) is **WinUSB**, then press *Replace Driver* (or
   *Install Driver*).
5. Back in LibreTune press *Scan*. The device should be listed.

## With STM32CubeProgrammer

Installing ST's STM32CubeProgrammer also installs a driver for `STM32 BOOTLOADER`; recent versions
bind WinUSB, while the old DfuSe tools installed ST's own `STTub30` driver, which LibreTune cannot
use. If LibreTune lists no device or says it is in use by another driver, switch it to WinUSB with
Zadig as above. STM32CubeProgrammer keeps working with WinUSB.

## Teensy (Speeduino)

The Teensy bootloader is a HID device. Windows binds its own HID driver to it, which LibreTune's
WinUSB backend cannot use; use PJRC's Teensy Loader on Windows, or flash from Linux or macOS.

## Undo

Device Manager > the device > Uninstall device (tick "delete the driver software" if offered), then
replug.
