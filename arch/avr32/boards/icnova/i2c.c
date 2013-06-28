/*
 * Board-setup of the I2C / TWI-sub-system
 *
 * (C) 2008 by Benjamin Tietz <benjamin.tietz@in-circuit.de>
 */

#include <linux/i2c.h>
#ifndef CONFIG_ATMEL_TWI
#include <mach/gpio.h>
#include <mach/at32ap700x.h>
#include <mach/portmux.h>
#include <linux/i2c-gpio.h>
#include <linux/platform_device.h>
#endif

static struct i2c_board_info icnova_i2c[] = {
#ifdef CONFIG_BOARD_ICNOVA_I2C_M41T82
	{
		.type = "m41t82",
		.addr = 0x68,
	},
#endif
#ifdef CONFIG_BOARD_LINUMATIC
	{
		.type = "pcf8563",
		.addr = 0x51,
	},
	{
		.type = "ad7995",
		.addr = 0x28,
	},
	{
		.type = "lm75",
		.addr = 0x48,
	},
#ifdef CONFIG_BOARD_LINUMATIC_4H
	{
		.type = "ad7994",
		.addr = 0x21,
	},
#endif
#endif
};

#ifndef CONFIG_ATMEL_TWI
static struct i2c_gpio_platform_data i2c_gpio_data = {
	.sda_pin		= GPIO_PIN_PA(6),
	.scl_pin		= GPIO_PIN_PA(7),
	.sda_is_open_drain	= 1,
	.scl_is_open_drain	= 1,
	.udelay			= 50, /* 2 kHz */
};

static struct platform_device i2c_gpio_device = {
	.name		= "i2c-gpio",
	.id		= 0,
	.dev		= {
		.platform_data = &i2c_gpio_data,
	},
};
#endif

static int __init icnova_setup_twi(void) {
#ifdef CONFIG_ATMEL_TWI
	at32_add_device_twi(0, icnova_i2c, ARRAY_SIZE(icnova_i2c));
#else
	at32_select_gpio(i2c_gpio_data.sda_pin,
		AT32_GPIOF_MULTIDRV | AT32_GPIOF_OUTPUT | AT32_GPIOF_HIGH);
	at32_select_gpio(i2c_gpio_data.scl_pin,
		AT32_GPIOF_MULTIDRV | AT32_GPIOF_OUTPUT | AT32_GPIOF_HIGH);
	platform_device_register(&i2c_gpio_device);
	i2c_register_board_info(0, icnova_i2c, ARRAY_SIZE(icnova_i2c));
#endif
	return 0;
}

arch_initcall(icnova_setup_twi);
