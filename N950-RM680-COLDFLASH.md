# Experimental RM-680 cold-flash support

This branch is based on Pali Rohar's upstream `0xFFFF` tag `0.10`, commit
`f6b0a7be3abb04ae61765c1890e8878c5762baf3`.  It adds the protocol used by
the Nokia N950 (RM-680) first-stage loader to start a signed APE flashing
algorithm after mask-ROM recovery.

## Status and safety

This is an experimental prerelease.  It is intended for an RM-680 that is
already stuck in OMAP mask-ROM USB mode (`0421:0106`, `Nokia USB ROM`).  It is
not a general Harmattan installer.

The supplied binary and source build reproducibly, and the protocol was
derived from the RM-680 loader and Nokia flasher traffic.  However, the final
patched `0xFFFF` binary has not yet completed an end-to-end cold boot through
APE re-enumeration on hardware.  The recovered HW-revision-1024 N950 used for
this work ultimately reached MeeGo/Harmattan after a signed cold full-flash
with Nokia's original flasher.  Do not treat that recovery as proof that this
fork is fully hardware-validated.

Cold flashing can brick the phone.  Use only signed `2nd` and `ape-algo`
images that match the exact RM-680 hardware revision.  Keep the battery
charged, do not disconnect the cable during a transfer, and retain a known
working recovery path.  No Nokia firmware or bootloader image is included.

## RM-680 changes

Upstream `0xFFFF 0.10` implements the RX-51 path: command `0x6302` followed by
a `secondary` image.  The RM-680 loader instead expects:

- command `0x6304` followed by the signed `ape-algo` image;
- acknowledgement `0x00006faa` after both the command and payload;
- 512-byte mask-ROM payload chunks and 64-KiB X-Loader payload writes;
- conservative delays between old OMAP3 USB transfers;
- a five-second timeout for the final acknowledgement.

The patch also reports the actual libusb result and handles positive short or
zero-byte payload writes.  It deliberately does not retry the four-byte 2nd
size word because retrying protocol words can desynchronise the loader.  On
some modern xHCI hosts that transfer remains a practical weak point.

## Build

Install a C compiler, GNU Make, and the libusb-0.1 compatibility development
files, then run:

```sh
make clean
make
./src/0xFFFF -h
```

The release binary is Linux x86-64, dynamically linked against
`libusb-0.1.so.4`, and requires glibc 2.38 or newer.  Building from source is
recommended on older distributions.

## Cold-bootstrap interface

After obtaining and verifying the matching signed images from firmware you
are legally entitled to use, the new interface is:

```sh
sudo ./src/0xFFFF \
  -m 2nd:/path/to/matching-2nd.bin \
  -m ape-algo:/path/to/matching-ape-algo.bin \
  -c -v
```

This transfers the two images into RAM; it does not by itself write a
persistent kernel or full firmware.  Stop if the detected USB identity,
device family, or image hardware revision does not match the target.

## License

This fork remains licensed under GNU GPL version 3 or, at your option, any
later version.  See `COPYING` and the copyright notices in the source files.
