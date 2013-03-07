/*
 * ICnova board-specific flash initialization
 *
 * Copyright (C) 2009 Benjamin Tietz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/physmap.h>

#include <mach/smc.h>
#include <mach/gpio.h>
#include <mach/board.h>
#include <mach/at32ap700x.h>
#include <mach/portmux.h>

static struct smc_timing nand_timing __initdata = {
	.ncs_read_setup		= 0,
	.nrd_setup		= 20,
	.ncs_write_setup	= 0,
	.nwe_setup		= 20,

	.ncs_read_pulse		= 100,
	.nrd_pulse		= 60,
	.ncs_write_pulse	= 100,
	.nwe_pulse		= 60,

	.read_cycle		= 120,
	.write_cycle		= 120,
};

static struct smc_config nand_config __initdata = {
	.bus_width		= 1,
	.nrd_controlled		= 1,
	.nwe_controlled		= 1,
	.tdf_cycles		= 2,
};

#define ROOT_SIZE	(16<<20)
#define KERNEL_SIZE	(2<<20)
static struct mtd_partition nand_parts[] = {
	{
		.name		= "kernel",
		.offset		= 0x0,
		.size		= KERNEL_SIZE,
		.mask_flags	= 0,
	},
	{
		.name		= "root",
		.offset		= KERNEL_SIZE,
		.size		= ROOT_SIZE,
		.mask_flags	= 0,
	},
	{
		.name           = "Data",
		.offset         = ROOT_SIZE + KERNEL_SIZE,
		.size           = 0x10000000 - ROOT_SIZE - KERNEL_SIZE,//256 MiB
		.mask_flags     = 0,
	},
};

static struct mtd_partition *nand_part_info(int size, int *num_partitions)
{
	*num_partitions = 3; //ARRAY_SIZE(nand_parts);
	return nand_parts;
}

static struct atmel_nand_data nand_data __initdata = {
#ifdef CONFIG_BOARD_ICNOVA_OEMPLUSBASED
	.enable_pin	= GPIO_PIN_PE(19),
	.rdy_pin	= GPIO_PIN_PE(23),
	.ale		= 21,
	.cle		= 22,
#endif
	.partition_info	= nand_part_info,
	.bus_width_16	= 0,
};

/* This needs to be called after the SMC has been initialized */
static int __init icnova_nand_init(void)
{
	int ret;

	pr_info("ICnova: registering NAND-Devices\n");
	smc_set_timing(&nand_config, &nand_timing);
	ret = smc_set_configuration(3, &nand_config);
	if (ret < 0) {
		printk(KERN_ERR "ICnova: failed to set NAND flash timing\n");
		return ret;
	}

#ifdef CONFIG_BOARD_ICNOVA_OEMPLUSBASED
	gpio_request(GPIO_PIN_PE(20), "nand.enable2");
	gpio_request(GPIO_PIN_PE(26), "nand.rdy2");
	gpio_direction_output(GPIO_PIN_PE(20), 1);
	gpio_direction_input(GPIO_PIN_PE(26));
#endif
	at32_add_device_nand(0, &nand_data);

	return 0;
}
device_initcall(icnova_nand_init);
