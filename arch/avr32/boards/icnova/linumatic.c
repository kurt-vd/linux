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
