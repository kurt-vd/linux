/*
 * init code specific for ICnova Base
 *
 * Copyright (C) 2007-2008 Benjamin Tietz <benjamin.tietz@in-circuit.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <mach/init.h>
#include <mach/board.h>

#include <linux/pm.h>
#include <linux/gpio.h>
#include <mach/at32ap700x.h>

/* define the poweroff gpio on linumatic */
#define POWEROFF_PIN GPIO_PIN_PA(26)

static void linumatic_pm_power_off(void)
{
	gpio_set_value(POWEROFF_PIN, 1);
}

static int __init linumatic_set_poweroff(void)
{
	if (gpio_request(POWEROFF_PIN, "linumatic poweroff") == 0) {
		gpio_direction_output(POWEROFF_PIN, 0);
		pm_power_off = linumatic_pm_power_off;
	}
	return 0;
}
late_initcall(linumatic_set_poweroff);

void __init setup_board(void)
{
	at32_map_usart(1, 0, 0);	// USART 1: /dev/ttyS0
	at32_setup_serial_console(0);
}

static int __init linumatic_init(void)
{
	at32_add_device_usart(0);
	return 0;
}

postcore_initcall(linumatic_init);
