/*
 * This code is just to set up the SPI-Interface, if it is needed
 *
 * (C) 2008 by Benjamin Tietz <benjamin.tietz@in-circuit.de>
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/bootmem.h>
#include <linux/spi/spi.h>
#include <linux/spi/ads7846.h>
#include <mach/board.h>
#include <mach/gpio.h>


#ifdef CONFIG_BOARD_ICNOVA_ADS7846
static struct ads7846_platform_data ads_info = {
	.model			= 7846,
	.vref_delay_usecs	= 100,
	.gpio_pendown		= CONFIG_BOARD_ICNOVA_ADS7846_IRQ,
	.x_min			= 330,
	.y_min			= 3700,
	.x_max			= 3700,
	.y_max			= 330,
	.settle_delay_usecs	= 50,
};

#endif

static struct spi_board_info spi0_board_info[] __initdata = {
#ifdef CONFIG_BOARD_ICNOVA_ADS7846
	{
		.modalias	= "ads7846",
		.max_speed_hz	= 125000 * 26,
		.chip_select	= CONFIG_BOARD_ICNOVA_ADS7846_CS,
		.platform_data	= &ads_info,
		.bus_num	= 0,
	},
#endif
#ifdef CONFIG_BOARD_ICNOVA_M25P80
	{
		.modalias	= "m25p80",
		.max_speed_hz	= 1000000,
		.chip_select	= CONFIG_BOARD_ICNOVA_M25P80_CS,
	},
#endif
#ifdef CONFIG_BOARD_ICNOVA_ZADCS1282
	{
		.modalias	= "zadcs1282",
		.max_speed_hz	= 3200000,
		.chip_select	= CONFIG_BOARD_ICNOVA_ZADCS1282_CS,
	},
#endif
};

static int __init icnova_add_spi(void) {
#ifdef CONFIG_BOARD_ICNOVA_ADS7846
	spi0_board_info[0].irq = gpio_to_irq(CONFIG_BOARD_ICNOVA_ADS7846_IRQ);
#endif
#ifdef CONFIG_BOARD_ICNOVA_ZADCS1282_SHDN
	gpio_direction_output(CONFIG_BOARD_ICNOVA_ZADCS1282_SHDN, 1);
#endif
	at32_add_device_spi(0, spi0_board_info, ARRAY_SIZE(spi0_board_info));
	return 0;
}

arch_initcall(icnova_add_spi);
