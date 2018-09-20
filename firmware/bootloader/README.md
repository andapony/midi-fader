# USB-Midi Fader Bootloader

The bootloader provides a mechanism for reprogramming the system without using
SWD beyond the first initial flashing of this image.

## Build instructions

Prerequisites:

 - arm-none-eabi-gcc
 - arm-none-eabi-binutils
 - python3

To build:

 1. Run `make` in this directory.

To flash the device:

 1. Connect the STLink to the device. Ensure your user has permissions to access
    it.
 2. Run `make install` in this directory.

To debug the device:

 1. Connect the STLink to the device. Ensure your user has permissions to access
    it.
 2. Run `make gdb` in this directory.

To gracefully halt the openocd process started during install or debug:

 1. Run `make stop` in this directory.

## Program Format

This bootloader lives in the first 8KB of program flash. Programs to be loaded
through this bootloader must have their sections located in the following
format:

```
Flash:
0x08002000: User program (.text, .rodata, etc)

Information block:
All sections may be used by the program
```

All writes and reads to the bootloader section (0x08000000-0x08001FFF,
0x08080000-0x080800FF) will be ignored and generate an appropriate status
report.

Firmware which writes to the first 256 bytes of EEPROM may cause the bootloader
to behave unexpectedly.

## Programming Verification

The bootloader includes automatic program verification as the device is
programmed in half-pages. However, it is still recommended to read back the
entire user section of program memory to verify that all blocks have been
written.

As an additional countermeasure against incorrect programming, once the device
enters bootloader mode, it will always enter bootloader mode on reset until
the "Exit Bootloader Mode" command is received. This prevents errors stemming
from device resets or loss of power during the programming process by allowing
the host software to start over programming without needing to resort to an
external reset.

## Protocol

### Entering Bootloader Mode

Bootloader is entered by simply performing any reset except a power-on reset.
If the power-on reset bit is set, the bootloader clears it and then jumps to the
user program, if it has been programmed.

### USB

The bootloader will appear as a USB device using the shared libusb VID/PID and
the name "LED Wristwatch Bootloader". It enumerates as an HID device and will
send and receive 64-byte reports. Each report encapsulates a command. As the
device is stateful, multiple host programs accessing the device at once may
cause corruption. The IN and OUT reports both utilize the default HID report ID,
so no HID report ID needs to be sent with the report.

The device will generate 64-byte IN reports on occasion to communicate the
device status. These reports have the following format:

```
Byte 0-3: First four bytes of the last command received or 0x00000000
Byte 4-7: Status flags
Byte 8-11: CRC32 of the lower page report to follow or just processed, if applicable
Byte 12-15: CRC32 of the upper page report to follow or just processed, if applicable
Byte 16-63: N/A
```

For an interpretation of the status flags, please see the `BootloaderError` enum
defined in `src/bootloader.c`. In general, these flags will only be set on
error and a `0` in the flags value can be interpreted as command success.

### CRC32 Implementation

The CRC32 is implemented using the hardware on the STM32. The host software will
use the `zlib` implementation of CRC32.

### Resetting the programming state machine

When the host program starts, a state machine reset command should be sent to
ensure the device is in a known state before performing any programming
commands. The reset command consists of sending the following report three
times:

The reset command has the following format:

```
Byte 0-63: 0x00
```

Upon receipt of the command, the device will generate a status report

### Exiting Bootloader Mode Before Programming

If the bootloader was started, but no programming operations have occurred and
the device was previously programmed, the user program can be started by issuing
the following command:

```
Byte 0: 0x3E
Byte 1-3: 0x00
Byte 4-63: N/A
```

Upon receipt of this command, the device will immediately reset and enter the
user program, if one has already been programmed and no new programming commands
have been received. Otherwise, a status report will be generated with an
appropriate error flag.

### Programming the Flash

Programming the flash requires 3 reports to be sent: A write/erase command a
two half-page OUT reports.

The write command has the following format:

```
Byte 0: 0x80
Byte 1-3: 0x00
Byte 4: Destination address bits 7-0 (bits 6-0 are ignored)
Byte 5: Destination address bits 15-8
Byte 6: Destination address bits 23-16
Byte 7: Destination address bits 31-24
Byte 8-11: CRC32 of the 16 32-bit words from the lower page command to follow, LSB first
Byte 12-15: CRC32 of the 16 32-bit words from the upper page command to follow, LSB first
Byte 16-63: N/A
```

Upon receipt of this command, the device will enter programming mode and
initiate an erase of the referenced block. A status report will be generated
once the erase has been completed.

The half-page command has the following format:

```
Byte 0-63: 16 32-bit words to program
```

Upon receipt of this command, the device will proceed to program the 16 words
starting at the destination address. In the event that less than a page needs
to be written, the remaining bytes should be filled with 0x00. When programming
has been completed, a status report will be generated and the device is ready to
receive the next page. Upon receipt of the final, upper page command, another
status report is generated and the device returns to the reset state.

Status commands during the programming phase will contain either a valid CRC32
for any section programmed, or zeros if the section has not been programmed. If
any error flags are set, then the device has returned to the reset state.

### Reading the Flash

Reading the flash requires 1 report to be sent and another to be received:
A read command and the page IN report.

The read command has the following format:

```
Byte 0: 0x40
Byte 1-3: 0x00
Byte 4: Source address bits 7-0 (bits 6-0 are ignored)
Byte 5: Source address bits 15-8
Byte 6: Source address bits 23-16
Byte 7: Source address bits 31-24
Byte 8-63: N/A
```

Upon receipt of this command, the device will generate two IN reports containing
the contents of the page. A status report will immediately follow and the device
returns to the reset state, being ready to accept the next command.

### Exiting Bootloader Mode After Programming

To exit bootloader mode, a report with the following format should be sent:

```
Byte 0: 0xC3
Byte 1-3: 0x00
Byte 4-7: VTOR value for the user program
Byte 8-63: N/A
```

The VTOR value should be the address at which the user program's interrupt
vector table resides. The first two words of the table are used to derive the
initial stack pointer and the location to jump to in order to initiate the user
program (reset vector).

Upon receipt of this command, the device will write the VTOR value to persistent
EEPROM and reset the device. If the VTOR value passed is invalid, or the device
is unable to write its EEPROM, it will send a status report detailing the
failure. Otherwise, it will initiate a device soft reset and the user program
will start on every power-on or low-power reset thereafter.

**This command must not be sent until the entire program has been written to
the flash.** In the event that the device stops programming before receiving
this command, it will automatically enter bootloader mode at the next reset
until it receives this command.

