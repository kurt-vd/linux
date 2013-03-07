/*
 * ICnova board-specific dm9010 initialization
 *
 * Copyright (C) 2009 by Benjamin Tietz <benjamin.tietz@in-circuit.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/dm9000.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include <mach/smc.h>
#include <mach/at32ap700x.h>

static struct smc_timing dm9010_timing __initdata = {
	.ncs_read_setup		= 0,
	.nrd_setup		= 0,
	.ncs_write_setup	= 0,
	.nwe_setup		= 0,

	.ncs_read_pulse		= 20,
	.nrd_pulse		= 20,
	.ncs_write_pulse	= 20,
	.nwe_pulse		= 20,

	.nrd_recover		= 100,
	.nwe_recover		= 60,
};

static struct smc_config dm9010_config __initdata = {
	.bus_width		= 2,
	.nrd_controlled		= 1,
	.nwe_controlled		= 1,
	.byte_write		= 0,
	.nwait_mode		= 3,
	.tdf_cycles		= 2,
};

static struct dm9000_plat_data dm9010_data = {
	.flags =
		DM9000_PLATF_16BITONLY|
		//DM9000_PLATF_32BITONLY|
		DM9000_PLATF_NO_EEPROM|
		DM9000_PLATF_SIMPLE_PHY,
};

static struct resource dm9010_resource[] = {
	{
		.start = 0x04000000 + 0x00,
		.end = 0x04000000 + 0x07,
		.flags = IORESOURCE_MEM,
	},
	{
		.start = 0x04000000 + 0x08,
		.end = 0x04000000 + 0x0F,
		.flags = IORESOURCE_MEM,
	},
	{
		.start = 64,
		.end = 64,
		.flags = IORESOURCE_IRQ|IORESOURCE_IRQ_LOWLEVEL,
	},
};

static struct platform_device dm9010_device = {
	.name		= "dm9000",
	.id		= 0,
	.resource	= dm9010_resource,
	.num_resources	= ARRAY_SIZE(dm9010_resource),
	.dev		= {
		.platform_data = &dm9010_data,
	},
};

/* This needs to be called after the SMC has been initialized */
static int __init icnova_dm9010_init(void)
{
	int ret;

	smc_set_timing(&dm9010_config, &dm9010_timing);
	ret = smc_set_configuration(4, &dm9010_config);
	if (ret < 0) {
		printk(KERN_ERR "ICnova: failed to set dm9010 timing\n");
		return ret;
	}

	gpio_direction_output(GPIO_PIN_PB(7), 1);
	gpio_direction_output(GPIO_PIN_PB(14), 1);
	mdelay(10);
	gpio_set_value(GPIO_PIN_PB(14), 0);

	platform_device_register(&dm9010_device);

	return 0;
}
device_initcall(icnova_dm9010_init);
