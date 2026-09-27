#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/bus.h>

#include <dev/spi/spivar.h>

#include <lib/libkern/libkern.h>
#include "bcmspi.h"

#define PIN_DC	24
#define PIN_RST	25
#define PIN_BL	18

#define TFT_W	480
#define TFT_H	320

struct tft_softc {
	struct device		sc_dev;
	spi_tag_t		sc_tag;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_gpioh;
};

int	 tft_match(struct device *, void *, void *);
void	 tft_attach(struct device *, struct device *, void *);
void	 tft_cmd(struct tft_softc *, uint8_t);

const struct cfattach tft_ca = {
	sizeof(struct tft_softc), tft_match, tft_attach
};

struct cfdriver tft_cd = {
	NULL, "tft", DV_DULL
};

/* fill.py init, 1:1. entry format: cmd, nbytes, delay_ms, data... */
static const uint8_t tft_initseq[] = {
	0x01, 0, 150,				/* soft reset */
	0x11, 0, 150,				/* sleep out */
	0xf1, 6, 0, 0x36, 0x04, 0x00, 0x3c, 0x0f, 0x8f,
	0xf2, 9, 0, 0x18, 0xa3, 0x12, 0x02, 0xb2, 0x12, 0xff, 0x10, 0x00,
	0xf8, 2, 0, 0x21, 0x04,
	0xf9, 2, 0, 0x00, 0x08,
	0xc0, 2, 0, 0x0f, 0x0f,
	0xc1, 1, 0, 0x44,
	0xc2, 1, 0, 0x33,
	0xc5, 4, 0, 0x00, 0x3c, 0x00, 0x00,
	0xb1, 2, 0, 0x90, 0x11,
	0xb4, 1, 0, 0x02,
	0xb6, 3, 0, 0x00, 0x42, 0x3b,
	0xb7, 1, 0, 0x07,
	0xe0, 15, 0, 0x0f, 0x1f, 0x1c, 0x0c, 0x0f, 0x08, 0x48, 0x98,
		0x37, 0x0a, 0x13, 0x04, 0x11, 0x0d, 0x00,
	0xe1, 15, 0, 0x0f, 0x32, 0x2e, 0x0b, 0x0d, 0x05, 0x47, 0x75,
		0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00,
	0xf7, 4, 0, 0xa9, 0x51, 0x2c, 0x82,
	0x36, 1, 0, 0x48,			/* MADCTL: MX | BGR */
	0x3a, 1, 0, 0x66,			/* 18 bpp RGB666, 3 bytes/pixel */
	0x21, 0, 0,				/* inversion on */
	0x29, 0, 50,				/* display on */
};

/* fill.py: row = bytes([0xFF, 0x00, 0x00]) * 480 */
static uint8_t tft_row[TFT_W * 3];

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

	/* GPIO.output(BL, 1) */
	bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPSET0, 1 << PIN_BL);

	/* reset low, sleep 1; reset high, sleep 1 */
	bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPCLR0, 1 << PIN_RST);
	delay(1000000);
	bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPSET0, 1 << PIN_RST);
	delay(1000000);

	/* spi.open(0, 0), 32 MHz, mode 0 */
	spi_acquire_bus(sc->sc_tag, 0);

	conf.sc_cs = 0;
	conf.sc_flags = 0;
	conf.sc_bpw = 8;
	conf.sc_freq = 8000000;
	conf.sc_cs_delay = 0;
	spi_config(sc->sc_tag, &conf);

	/* for cmd, data, delay in init */
	for (i = 0; i < (int)sizeof(tft_initseq); ) {
		tft_cmd(sc, tft_initseq[i]);
		n = tft_initseq[i + 1];
		if (n)
			spi_write(sc->sc_tag, (char *)&tft_initseq[i + 3], n);
		if (tft_initseq[i + 2])
			delay(tft_initseq[i + 2] * 1000);
		i += 3 + n;
	}

	/* DC=0 [0x2A]; DC=1 [0x00, 0x00, 0x01, 0xDF] */
	tft_cmd(sc, 0x2a);
	w[0] = 0x00; w[1] = 0x00; w[2] = 0x01; w[3] = 0xdf;
	spi_write(sc->sc_tag, (char *)w, 4);
	/* DC=0 [0x2B]; DC=1 [0x00, 0x00, 0x01, 0x3F] */
	tft_cmd(sc, 0x2b);
	w[0] = 0x00; w[1] = 0x00; w[2] = 0x01; w[3] = 0x3f;
	spi_write(sc->sc_tag, (char *)w, 4);
	/* DC=0 [0x2C] */
	tft_cmd(sc, 0x2c);

	/* row = bytes([0xFF, 0x00, 0x00]) * 480 */
	for (i = 0; i < TFT_W * 3; i += 3) {
		tft_row[i] = 0xff;
		tft_row[i + 1] = 0x00;
		tft_row[i + 2] = 0x00;
	}

	/* DC=1 (tft_cmd left it high); for i in range(320): writebytes(row) */
	for (i = 0; i < TFT_H; i++)
		spi_write(sc->sc_tag, (char *)tft_row, sizeof(tft_row));

	spi_release_bus(sc->sc_tag, 0);

	printf("\n");
}
