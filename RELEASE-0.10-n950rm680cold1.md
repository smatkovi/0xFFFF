# 0xFFFF 0.10-n950rm680cold1

Experimental Nokia N950 RM-680 cold-flash support based on upstream `0xFFFF`
0.10.

The fork adds RM-680's `0x6304` APE-algorithm command, validates `0x6faa`
acknowledgements, uses transfer sizes and delays suited to the OMAP3 loader,
and improves USB short-write diagnostics and handling.

The x86-64 binary was rebuilt cleanly and reproduced byte-for-byte.  It is
dynamically linked against `libusb-0.1.so.4` and needs glibc 2.38 or newer.
The complete corresponding GPL source is the tagged repository source.

This release is intentionally marked as a prerelease.  It has not yet
completed an end-to-end RM-680 cold bootstrap through APE re-enumeration with
this binary.  The successful HW-1024 recovery that motivated the patch used
Nokia's original flasher for the final signed cold full-flash.  Read
`N950-RM680-COLDFLASH.md` before use.

No Nokia firmware, bootloader, APE image, device log, or device identifier is
included.
