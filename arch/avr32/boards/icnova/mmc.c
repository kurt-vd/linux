/*
 * Board-initialization code for the MMC-Interface
 *
 * (C) 2008 by Benjamin Tietz <benjamin.tietz@in-circuit.de>
 */

#include <linux/init.h>
#include <linux/atmel-mci.h>
#include <mach/board.h>
#include <asm/errno.h>
#include <mach/at32ap700x.h>
#include <mach/gpio.h>

static struct mci_platform_data __initdata mci0_data = {
	// TODO Setup DMA?
	.slot[0] = {
#if defined(CONFIG_BOARD_ICNOVA_ADB1000) || defined(CONFIG_BOARD_ICNOVA_ADB1001)
		.detect_pin	= GPIO_PIN_PA(16),
#else
		.detect_pin	= -ENODEV,
#endif
		.wp_pin		= -ENODEV,
		.bus_width	= 4,
	},
};

static int __init icnova_setup_mmc(void) {

#ifdef CONFIG_BOARD_ICNOVA_ADB1001
	gpio_request(GPIO_PIN_PA(21), "mmi.sw2");
	gpio_direction_output(GPIO_PIN_PA(21), 0);
#endif
	at32_add_device_mci(0, &mci0_data);
	return 0;
}

arch_initcall(icnova_setup_mmc);
