#include <machine/bus.h>
#include <dev/spi/spivar.h>

/* BCM2711 GPIO block (Pi 4 peripherals base = 0xfe000000) */
#define GPIO_BASE	0xfe200000
#define GPIO_GPFSEL0	0x00
#define GPIO_GPFSEL1	0x04
#define GPIO_GPFSEL2	0x08
#define GPIO_GPSET0	0x1c
#define GPIO_GPCLR0	0x28

struct bcmspi_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_space_handle_t	sc_gpioh;
	struct rwlock		sc_buslock;
	struct spi_controller	sc_tag;
	uint32_t		sc_csmode;
};
