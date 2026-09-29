#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/ioctl.h>

#include <machine/bus.h>

#include <dev/spi/spivar.h>

#include <lib/libkern/libkern.h>
#include "img.h"
#include "bcmspi.h"

#define PIN_DC  24
#define PIN_RST 25
#define PIN_BL  18

#define TFT_W   480
#define TFT_H   320

#define TFTIOCBACKLIGHT _IOW('T', 0, int)   /* arg: 0 = off, 1 = on */

struct tft_softc {
        struct device           sc_dev;
        spi_tag_t               sc_tag;
        bus_space_tag_t         sc_iot;
        bus_space_handle_t      sc_gpioh;
        uint8_t                 *sc_fb;
        int                     sc_open;
        struct mutex             sc_mtx;
};

int      tft_match(struct device *, void *, void *);
void     tft_attach(struct device *, struct device *, void *);
int      tft_detach(struct device *, int);
void     tft_cmd(struct tft_softc *, uint8_t);
int      tftopen(dev_t, int, int, struct proc *);
int      tftclose(dev_t, int, int, struct proc *);
int      tftread(dev_t, struct uio *, int);
int      tftwrite(dev_t, struct uio *, int);
int      tftioctl(dev_t, u_long, caddr_t, int, struct proc *);
int      tftstop(struct tty *, int);
struct tty *tfttty(dev_t);
int      tftpoll(dev_t, int, struct proc *);
paddr_t  tftmmap(dev_t, off_t, int);

const struct cfattach tft_ca = {
        sizeof(struct tft_softc), tft_match, tft_attach, tft_detach
};

struct cfdriver tft_cd = {
        NULL, "tft", DV_DULL
};

/* ILI9486 init for the 74HC4094 clone boards: gamma + power settings
 * are required, the bare 0x11/0x29 sequence leaves the panel half-awake */
static const uint8_t tft_initseq[] = {
        0xe0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0a, 0x3f, 0x78, 0x4c, 0x09,
            0x0a, 0x08, 0x16, 0x1a, 0x0f,
        0xe1, 15, 0x00, 0x16, 0x19, 0x03, 0x0f, 0x05, 0x32, 0x45, 0x46, 0x04,
            0x0e, 0x0d, 0x35, 0x37, 0x0f,
        0xc0, 2, 0x17, 0x15,
        0xc1, 1, 0x41,
        0xc5, 3, 0x00, 0x12, 0x80,
        0x36, 1, 0x48,           /* MADCTL: MX | BGR */
        0x3a, 1, 0x55,           /* 16 bpp RGB565 */
        0xb0, 1, 0x00,
        0xb1, 1, 0xa0,
        0xb4, 1, 0x02,
        0xb6, 3, 0x02, 0x02, 0x3b,
        0xb7, 1, 0xc6,
        0xf7, 4, 0xa9, 0x51, 0x2c, 0x82,
};

int
tft_match(struct device *parent, void *match, void *aux)
{
        struct spi_attach_args *sa = aux;

        return strcmp(sa->sa_name, "tft") == 0;
}

void
tft_cmd(struct tft_softc *sc, uint8_t cmd)
{
        bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPCLR0, 1 << PIN_DC);
        spi_write(sc->sc_tag, (char *)&cmd, 1);
        bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPSET0, 1 << PIN_DC);
}

void
tft_attach(struct device *parent, struct device *self, void *aux)
{
        struct tft_softc *sc = (struct tft_softc *)self;
        struct spi_attach_args *sa = aux;
        struct bcmspi_softc *psc = sa->sa_cookie;
        struct spi_config conf;
        uint8_t w[4];
        int i, n;

        sc->sc_tag = sa->sa_tag;
        sc->sc_iot = psc->sc_iot;
        sc->sc_gpioh = psc->sc_gpioh;
        mtx_init(&sc->sc_mtx, IPL_NONE);
        sc->sc_fb = malloc(TFT_W * TFT_H * 2, M_DEVBUF, M_WAITOK);

        /* backlight on, then hardware reset pulse */
        bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPSET0, 1 << PIN_BL);
        bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPCLR0, 1 << PIN_RST);
        delay(100000);
        bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPSET0, 1 << PIN_RST);
        delay(120000);

        spi_acquire_bus(sc->sc_tag, 0);

        conf.sc_cs = 0;             /* CE0 = GPIO 8 */
        conf.sc_flags = 0;          /* SPI mode 0 */
        conf.sc_bpw = 8;
        conf.sc_freq = 4000000;
        conf.sc_cs_delay = 0;
        spi_config(sc->sc_tag, &conf);

        for (i = 0; i < (int)sizeof(tft_initseq); ) {
                tft_cmd(sc, tft_initseq[i]);
                n = tft_initseq[i + 1];
                if (n)
                        spi_write(sc->sc_tag, (char *)&tft_initseq[i + 2], n);
                i += 2 + n;
        }

        tft_cmd(sc, 0x11);          /* sleep out */
        delay(120000);
        tft_cmd(sc, 0x29);          /* display on */

        tft_cmd(sc, 0x2a);          /* column address 0..479 */
        w[0] = 0; w[1] = 0; w[2] = (TFT_W - 1) >> 8; w[3] = (TFT_W - 1) & 0xff;
        spi_write(sc->sc_tag, (char *)w, 4);
        tft_cmd(sc, 0x2b);          /* page address 0..319 */
        w[2] = (TFT_H - 1) >> 8; w[3] = (TFT_H - 1) & 0xff;
        spi_write(sc->sc_tag, (char *)w, 4);
        tft_cmd(sc, 0x2c);          /* memory write */

        memcpy(sc->sc_fb, frame_raw, sizeof(frame_raw));
        spi_write(sc->sc_tag, (char *)sc->sc_fb, TFT_W * TFT_H );

        spi_release_bus(sc->sc_tag, 0);

        printf("\n");
}

/* writes must be exactly one full RGB565 frame: 480*320*2 = 307200 bytes */
int
tftopen(dev_t dev, int flag, int mode, struct proc *p)
{
        struct tft_softc *sc;
        struct device *dv;

        dv = device_lookup(&tft_cd, minor(dev));
        if (dv == NULL)
                return ENXIO;
        sc = (struct tft_softc *)dv;
        if (sc->sc_open)
                return EBUSY;
        sc->sc_open = 1;
        return 0;
}

int
tftclose(dev_t dev, int flag, int mode, struct proc *p)
{
        struct tft_softc *sc =
            (struct tft_softc *)device_lookup(&tft_cd, minor(dev));

        if (sc != NULL)
                sc->sc_open = 0;
        return 0;
}

int
tftwrite(dev_t dev, struct uio *uio, int flags)
{
        struct tft_softc *sc =
            (struct tft_softc *)device_lookup(&tft_cd, minor(dev));
        int error;

        if (sc == NULL)
                return ENXIO;
        if (uio->uio_resid != TFT_W * TFT_H * 2)
                return EINVAL;

        mtx_enter(&sc->sc_mtx);
        error = uiomove(sc->sc_fb, TFT_W * TFT_H * 2, uio);
        if (error == 0) {
                spi_acquire_bus(sc->sc_tag, 0);
                tft_cmd(sc, 0x2c);      /* memory write, back to pixel 0 */
                spi_write(sc->sc_tag, (char *)sc->sc_fb, TFT_W * TFT_H * 2);
                spi_release_bus(sc->sc_tag, 0);
        }
        mtx_leave(&sc->sc_mtx);
        return error;
}

int
tftioctl(dev_t dev, u_long cmd, caddr_t addr, int flag, struct proc *p)
{
        struct tft_softc *sc =
            (struct tft_softc *)device_lookup(&tft_cd, minor(dev));
        int on;

        if (sc == NULL)
                return ENXIO;
        switch (cmd) {
        case TFTIOCBACKLIGHT:
                on = *(int *)addr;
                bus_space_write_4(sc->sc_iot, sc->sc_gpioh,
                    on ? GPIO_GPSET0 : GPIO_GPCLR0, 1 << PIN_BL);
                return 0;
        default:
                return ENOTTY;
        }
}

int
tftread(dev_t dev, struct uio *uio, int flags)
{
        return ENODEV;
}

int
tftstop(struct tty *tp, int flag)
{
        return 0;
}

struct tty *
tfttty(dev_t dev)
{
        return NULL;
}

int
tftpoll(dev_t dev, int events, struct proc *p)
{
        return 0;
}

paddr_t
tftmmap(dev_t dev, off_t off, int prot)
{
        return -1;
}

int
tft_detach(struct device *self, int flags)
{
        struct tft_softc *sc = (struct tft_softc *)self;

        bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPCLR0, 1 << PIN_BL);
        if (sc->sc_fb != NULL)
                free(sc->sc_fb, M_DEVBUF, TFT_W * TFT_H * 2);
        return 0;
}
