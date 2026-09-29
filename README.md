# OpenBSD TFT Framebuffer Kernel Driver (v1.0)

A custom OpenBSD/arm64 kernel that turns a cheap ILI9486 SPI TFT into
`/dev/tft0`:

```
dd if=frame.rgb565 of=/dev/tft0 bs=307200 count=1    # image appears on the screen
```

This is **deliberately not** a wsdisplay/fbdev integration. It's a ~300-line
raw character device driver, written to be read and hacked on. One full frame
(480×320 RGB565) per write, no readback, no mmap.

Target: OpenBSD/arm64 on a Raspberry Pi (BCM2835 SPI). Developed against
OpenBSD 7.x sources.

## How it works

```
userspace   dd if=frame.rgb565 of=/dev/tft0
                │ open(2) → major/minor from the mknod
                ▼
cdevsw      sys/arch/arm64/arm64/conf.c   (tft device entries)
                │ device_lookup(minor)
                ▼
tft         sys/dev/fdt/tft.c             (frame buffer, ILI9486 init)
                │ spi_write + GPIO bit-bang (DC/RST/BL)
                ▼
bcmspi      sys/dev/fdt/bcmspi.c          (SPI bus, GPIO, hand-attach)
                │ polling PIO transfer
                ▼
hardware    BCM2835 SPI0 + GPIO → ILI9486 TFT (480×320, RGB565)
```

The kernel config (`TFT`) compiles the driver in; the `mknod` creates the
userland name.

## The interesting part: no device tree needed

The Pi device tree has no node for an SPI display, and OpenBSD can't load
device tree overlays. So `tft` never looks at the FDT — instead, the last
thing `bcmspi_attach()` does is fabricate an `spi_attach_args` and adopt the
display by hand:

```c
/* fixed child: our display (Pi DT has no spi child nodes) */
sa.sa_name = "tft";
sa.sa_cookie = sc;
config_found(self, &sa, NULL);
```

The SPI parent also maps the GPIO block (it sits 0x4000 below SPI0), sets up
the display's control pins, and locks the bus — so `tft.c` just uses what's
already there. Pi-specific by construction; on another board you'd redo this
bootstrap in that SoC's SPI driver.

## Repository layout

```
openbsdRaspiTFTKernelDriver/
├── README.md                 ← this file
├── mkframe.py                 ← PNG/JPG → RGB565 frame converter
├── unnamed.jpg                ← the screen actually working
└── usr/src/sys/
    ├── arch/arm64/
    │   ├── arm64/conf.c       ← modified: tft registered at major 101
    │   └── conf/
    │       └── TFT            ← kernel config
    └── dev/fdt/
        ├── tft.c              ← framebuffer driver
        ├── img.h              ← splash image (frame_raw), compiled in
        ├── bcmspi.c           ← SPI bus driver (maps GPIO, hand-attaches tft)
        └── bcmspi.h           ← shared softc/GPIO definitions
```

## Hardware

- Raspberry Pi (developed/tested on Pi 4, BCM2711)
- 3.5" 480×320 SPI TFT with ILI9486, on the 40-pin header — the common
  "74HC4094 shift-register clone" board (the init sequence's gamma/power
  settings are tuned for it; a bare `0x11`/`0x29` sequence leaves these
  panels half-awake)

### Wiring

| TFT pin | Pi GPIO | header pin | Notes |
|---|---|---|---|
| VCC | 3.3V | 1 | |
| GND | GND | 6 | |
| MOSI | GPIO 10 | 19 | |
| MISO | GPIO 9 | 21 | unused, panel SDO not wired on most clones |
| SCLK | GPIO 11 | 23 | |
| CS | GPIO 8 (CE0) | 24 | `conf.sc_cs = 0` in tft.c |
| DC | GPIO 24 | 18 | bit-banged by tft |
| RST | GPIO 25 | 22 | bit-banged by tft |
| BL | GPIO 18 | 12 | backlight, on at attach |

Pins are hardcoded as `PIN_DC/PIN_RST/PIN_BL` in `tft.c` and in the FSEL
writes in `bcmspi.c` — change both if you rewire.

## Build

Prereqs: OpenBSD/arm64 with source tree matching your release (install the
`src` set, or `cvs checkout -rOPENBSD_7_<x> src` into `/usr/src`).

```sh
# 1. copy kernel config
cp usr/src/sys/arch/arm64/conf/TFT /usr/src/sys/arch/arm64/conf/TFT

# 2. copy the drivers and the modified conf.c
cp usr/src/sys/dev/fdt/tft.c usr/src/sys/dev/fdt/img.h \
   usr/src/sys/dev/fdt/bcmspi.c usr/src/sys/dev/fdt/bcmspi.h \
   /usr/src/sys/dev/fdt/
cp usr/src/sys/arch/arm64/arm64/conf.c /usr/src/sys/arch/arm64/arm64/conf.c

# 3. register the character device in the kernel's device switch.
#    Use the modified conf.c from this repo (usr/src/sys/arch/arm64/arm64/conf.c)
#    — two edits vs. stock. First, with the other cdev_decl lines:
#
#        cdev_decl(tft);
#
#    then in the cdevsw[] table, right after the joystick entry:
#
#        cdev_ujoy_init(NUJOY,ujoy), /* 100: USB joystick/gamecontroller */
#        cdev_disk_init(1,tft),      /* 101: TFT LCD */
#
#    (majors are positional — the joystick sits at 100, so tft lands at 101)

# 4. build
cd /sys/arch/arm64/compile/TFT
make obj && make config && make -j$(sysctl -n hw.ncpu)
```

## Install

```sh
doas cp /bsd /bsd.sp          # fallback: type "boot /bsd.sp" at the boot prompt
doas make install
doas reboot
```

Verify:

```
dmesg | grep -E 'tft|bcmspi'
```

`bcmspi` attaches and maps GPIO, then `tft` attaches, resets the panel, runs
the ILI9486 init, and shows the compiled-in splash image.

## Creating /dev/tft0

OpenBSD has no devfs — the node is made by hand:

```sh
doas mknod /dev/tft0 c 101 0
doas chmod 666 /dev/tft0    # or tighten ownership to taste
```

Major **101** is `tft`'s position in the `cdevsw` table (inserted after the
joystick at 100 — see the Build step), minor **0** is the unit. Confirm with
`ls -l /dev/tft0` on your built kernel; if your insertion point differs, use
whatever major it shows.

## Using the driver

Writes must be **exactly one full frame**: 480 × 320 × 2 = 307,200 bytes of
little-endian RGB565, no header. Anything else returns `EINVAL`. The `dd`
form below guarantees that.

```sh
# 1. convert any image to a frame (uses the repo's mkframe.py)
python3 mkframe.py input.png frame.rgb565

# 2. push it to the display
dd if=frame.rgb565 of=/dev/tft0 bs=307200 count=1
```

(`cat frame.rgb565 > /dev/tft0` works too — the write just has to be one
full 307,200-byte frame.)

Backlight ioctl (see `tft.c`):

```c
int on = 1;
ioctl(fd, TFTIOCBACKLIGHT, &on);
```

Boot-time splash via `/etc/rc.local`:

```sh
if [ -c /dev/tft0 ]; then
    dd if=/home/devil/frame.rgb565 of=/dev/tft0 bs=307200 count=1
fi
```

## TODO

- **Touchscreen support.** These 3.5" clones carry an XPT2046/ADS7846
  touch controller on the same SPI bus (usually MISO pin 21, interrupt on
  GPIO or shared PENIRQ). Plan: attach it as a second `spi` child from
  `bcmspi` (same hand-attach trick as `tft`), implement an OpenBSD
  `wsmux`-compatible `wskbd`/`wsmouse`-style device or a simple character
  device reporting raw X/Y/Z, and expose calibration via ioctl.
- Readback / partial-frame writes (currently write-only, whole-frame).
- mmap interface for shared-memory drawing.
- Faster SPI clock once the transfer loop is DMA-capable.

## Limitations

- Write-only: no readback, no mmap. Exactly one frame per write, single
  opener (`EBUSY` otherwise).
- SPI at 4 MHz polled PIO — full-frame writes take a moment; this is a
  proof-of-concept, not a video path.
- Pi-only by design (hand-attach in `bcmspi`); porting = redoing that
  bootstrap on another SoC's SPI driver.
- No touchscreen (see TODO).
- `img.h` embeds a splash image; replace it with your own artwork if
  redistributing.

## License

ISC, same as OpenBSD.
