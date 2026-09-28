### OpenBSD 7.9 ARM64 TFT driver for ILI9486 TFT screen

Kernel driver that displays a static image on a 3.5" 480x320 SPI TFT
module (ILI9486-based "clone" boards, e.g. Waveshare 3.5" / keidei type)
attached to a Raspberry Pi running OpenBSD/arm64.

Current version does not support the touchscreen; it only puts an image
(compiled into the kernel) onto the screen.

### Hardware

- Raspberry Pi (developed/tested on Pi 4, BCM2711)
- 3.5" 480x320 SPI TFT with ILI9486, on the 40-pin header
- Wiring used by the driver:

  | signal | GPIO | header pin |
  |---|---|---|
  | MOSI | 10 | 19 |
  | MISO | 9  | 21 (unused, panel SDO not wired on most clones) |
  | SCLK | 11 | 23 |
  | CS (CE0) | 8 | 24 |
  | DC | 24 | 18 |
  | RST | 25 | 22 |
  | BL | 18 | 12 |

### Repository contents

- `bcmspi.c` / `bcmspi.h` — SPI0 controller driver (BCM2835/BCM2711)
- `tft.c` — ILI9486 display driver, paints `frame_raw` at attach
- `img.h` — the image, as a `frame_raw[]` byte array (RGB565)
- `TFT` — example kernel config

### Build steps

1. Install OpenBSD 7.9 arm64 on the Pi.

2. Fetch and extract the system sources (as root):
   ```
   cd /
   ftp https://cdn.openbsd.org/pub/OpenBSD/7.9/src.tar.gz
   ftp https://cdn.openbsd.org/pub/OpenBSD/7.9/sys.tar.gz
   tar xzf src.tar.gz
   tar xzf sys.tar.gz
   ```

3. Enable SPI in the Pi firmware (boot partition `config.txt`):
   ```
   dtparam=spi=on
   ```

4. Copy the driver files:
   ```
   cp bcmspi.c bcmspi.h tft.c img.h /usr/src/sys/dev/fdt/
   ```

5. Add the device declarations so the build and autoconfig know the
   drivers (this is the easy-to-forget part). In
   `/usr/src/sys/arch/arm64/conf/files.arm64` add:
   ```
   device	bcmspi: spi
   attach	bcmspi at fdt
   device	tft
   attach	tft at bcmspi
   ```

6. Create the kernel config `/usr/src/sys/arch/arm64/conf/TFT`:
   ```
   include "../conf/GENERIC"

   bcmspi* at fdt?
   tft*	at bcmspi?
   ```

7. Generate the build directory and compile:
   ```
   cd /sys/arch/arm64/conf
   config TFT
   cd /sys/arch64/compile/TFT
   make -j4
   ```

8. Install and reboot:
   ```
   cp /bsd /bsd.orig
   cp bsd /bsd
   reboot
   ```

9. Verify it attached:
   ```
   dmesg | grep -E 'bcmspi|tft'
   ```
   Expected:
   ```
   bcmspi0 at simplebus0: SPI0 controller
   tft0 at bcmspi0
   ```

### Changing the image

`img.h` must be exactly 320*480*2 = 307200 bytes of RGB565, big-endian,
row by row (portrait 320 wide x 480 tall, matching MADCTL 0x48).
Generate it from any JPEG with `mkimg.py` (needs `pillow` and `numpy`):

   [ the generator script ]

Then rebuild and reinstall the kernel as above.

For landscape 480x320 instead: set MADCTL to 0x68 in the init table of
tft.c (adds the MV bit), regenerate the image as 480x320, and swap
TFT_W/TFT_H.

### Troubleshooting

- `dmesg` shows no `bcmspi0`: SPI node disabled in firmware -> check
  `dtparam=spi=on`; or kernel was built without the config lines in
  step 5/6.
- `bcmspi0` present but no `tft0`: `config_found` path broken -> check
  `tft* at bcmspi?` in the kernel config.
- White screen, both lines in dmesg: panel is initialized but data is
  garbled -> check `img.h` is exactly 307200 bytes; check the MADCTL
  orientation note above.
- Kernel panics or weirdness after attach: make sure the memcpy in
  tft_attach is clamped to the framebuffer size.

### Known quirks / porting notes

- The BCM2835/2711 SPI FIFO is word-granular: every 32-bit access to
  SPI_FIFO shifts 4 bytes. bcmspi_transfer() packs bytes into words,
  MSB first. Keep transfers a multiple of 4 bytes.
- Most clone panels do not wire the ILI9486 SDO pin to the MISO header
  pin, so register readback (e.g. Read ID) returns zeros. This is a
  hardware limitation, not a driver bug.
- The whole screen is repainted from a 300 KB malloc'd framebuffer in
  kernel memory at every boot.

### TODO / roadmap

- character device `/dev/tft0` so images can be pushed from userland
- partial-window updates instead of full-frame repaint
- XPT2046 / ADS7846 touchscreen support
