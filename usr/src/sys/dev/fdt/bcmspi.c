#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/rwlock.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/fdt.h>
#include <dev/spi/spivar.h>

#include "bcmspi.h"

/* BCM2835 SPI registers */
#define SPI_CS		0x00
#define  SPI_CS_CS	0x00000003
#define  SPI_CS_CPHA	0x00000004
#define  SPI_CS_CPOL	0x00000008
#define  SPI_CS_CLEAR	0x00000030
#define  SPI_CS_TA	0x00000080
#define  SPI_CS_DONE	0x00010000
#define  SPI_CS_RXD	0x00020000
#define  SPI_CS_TXD	0x00040000
#define SPI_FIFO	0x04
#define SPI_CLK		0x08

#define GPIO_FSEL_ALT0	0x4
#define GPIO_FSEL_OUT	0x1

#define SPI_CORE_CLK	250000000

#define HREAD4(sc, reg)							\
	(bus_space_read_4((sc)->sc_iot, (sc)->sc_ioh, (reg)))
#define HWRITE4(sc, reg, val)						\
	(bus_space_write_4((sc)->sc_iot, (sc)->sc_ioh, (reg), (val)))

int	 bcmspi_match(struct device *, void *, void *);
void	 bcmspi_attach(struct device *, struct device *, void *);
void	 bcmspi_config(void *, struct spi_config *);
int	 bcmspi_transfer(void *, char *, char *, int, int);
int	 bcmspi_acquire_bus(void *, int);
void	 bcmspi_release_bus(void *, int);

const struct cfattach bcmspi_ca = {
	sizeof(struct bcmspi_softc), bcmspi_match, bcmspi_attach
};

struct cfdriver bcmspi_cd = {
	NULL, "bcmspi", DV_DULL
};

int
bcmspi_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "brcm,bcm2835-spi");
}

void
bcmspi_attach(struct device *parent, struct device *self, void *aux)
{
	struct bcmspi_softc *sc = (struct bcmspi_softc *)self;
	struct fdt_attach_args *faa = aux;
	struct spi_attach_args sa;
	uint32_t fsel;

	if (faa->fa_nreg < 1) {
		printf(": no registers\n");
		return;
	}
	sc->sc_iot = faa->fa_iot;
	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr,
	    faa->fa_reg[0].size, 0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}
	/* GPIO block sits 0x4000 below SPI0, same bus addressing */
	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr - 0x4000,
	    0x100, 0, &sc->sc_gpioh)) {
		printf(": can't map gpio\n");
		return;
	}
	/* pins 7,8,9 (CE1/CE0/MISO) to ALT0 */
	fsel = bus_space_read_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPFSEL0);
	fsel &= ~((7 << 21) | (7 << 24) | (7 << 27));
	fsel |= (GPIO_FSEL_ALT0 << 21) | (GPIO_FSEL_ALT0 << 24) |
	    (GPIO_FSEL_ALT0 << 27);
	bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPFSEL0, fsel);

	/* pins 10,11 (MOSI/CLK) to ALT0, pin 18 (BL) to output */
	fsel = bus_space_read_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPFSEL1);
	fsel &= ~((7 << 0) | (7 << 3) | (7 << 24));
	fsel |= (GPIO_FSEL_ALT0 << 0) | (GPIO_FSEL_ALT0 << 3) |
	    (GPIO_FSEL_OUT << 24);
	bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPFSEL1, fsel);

	/* pins 24 (DC), 25 (RST) to output */
	fsel = bus_space_read_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPFSEL2);
	fsel &= ~((7 << 12) | (7 << 15));
	fsel |= (GPIO_FSEL_OUT << 12) | (GPIO_FSEL_OUT << 15);
	bus_space_write_4(sc->sc_iot, sc->sc_gpioh, GPIO_GPFSEL2, fsel);

	rw_init(&sc->sc_buslock, sc->sc_dev.dv_xname);

	sc->sc_tag.sc_cookie = sc;
	sc->sc_tag.sc_config = bcmspi_config;
	sc->sc_tag.sc_transfer = bcmspi_transfer;
	sc->sc_tag.sc_acquire_bus = bcmspi_acquire_bus;
	sc->sc_tag.sc_release_bus = bcmspi_release_bus;

	printf("\n");

	/* fixed child: our display (Pi DT has no spi child nodes) */
	memset(&sa, 0, sizeof(sa));
	sa.sa_tag = &sc->sc_tag;
	sa.sa_name = "tft";
	sa.sa_cookie = sc;
	config_found(self, &sa, NULL);
}

void
bcmspi_config(void *cookie, struct spi_config *conf)
{
	struct bcmspi_softc *sc = cookie;
	uint32_t div;

	if (conf->sc_cs > 1) {
		printf("%s: invalid chip-select (%d)\n",
		    sc->sc_dev.dv_xname, conf->sc_cs);
		return;
	}

	sc->sc_csmode = conf->sc_cs;
	if (conf->sc_flags & SPI_CONFIG_CPOL)
		sc->sc_csmode |= SPI_CS_CPOL;
	if (conf->sc_flags & SPI_CONFIG_CPHA)
		sc->sc_csmode |= SPI_CS_CPHA;

	/* CDIV must be a power of two; keep clock <= requested */
	div = 2;
	while (SPI_CORE_CLK / div > conf->sc_freq)
		div <<= 1;
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, SPI_CLK, div);
}

int
bcmspi_transfer(void *cookie, char *out, char *in, int len, int flags)
{
	struct bcmspi_softc *sc = cookie;
	uint32_t cs;
	int i;

	HWRITE4(sc, SPI_CS, sc->sc_csmode | SPI_CS_CLEAR);
	HWRITE4(sc, SPI_CS, sc->sc_csmode | SPI_CS_TA);

	for (i = 0; i < len; i++) {
		if (in) {
			while (!(HREAD4(sc, SPI_CS) & SPI_CS_RXD))
				;
			in[i] = HREAD4(sc, SPI_FIFO) & 0xff;
		}
		if (out) {
			/* drain echoed bytes while waiting, else we stall */
			while (!(HREAD4(sc, SPI_CS) & SPI_CS_TXD)) {
				if (HREAD4(sc, SPI_CS) & SPI_CS_RXD)
					(void)HREAD4(sc, SPI_FIFO);
			}
			HWRITE4(sc, SPI_FIFO, (uint8_t)out[i]);
		}
	}

	/* empty RX, then wait for the shifter to finish */
	for (;;) {
		cs = HREAD4(sc, SPI_CS);
		if (cs & SPI_CS_RXD)
			(void)HREAD4(sc, SPI_FIFO);
		else if (cs & SPI_CS_DONE)
			break;
	}

	if (!ISSET(flags, SPI_KEEP_CS))
		HWRITE4(sc, SPI_CS, sc->sc_csmode);

	return 0;
}
int
bcmspi_acquire_bus(void *cookie, int flags)
{
	struct bcmspi_softc *sc = cookie;

	rw_enter(&sc->sc_buslock, RW_WRITE);
	return 0;
}

void
bcmspi_release_bus(void *cookie, int flags)
{
	struct bcmspi_softc *sc = cookie;

	rw_exit(&sc->sc_buslock);
}
