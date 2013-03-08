/*
 * init code specific for ICnova OEM
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
	at32_map_usart(0, 0, 0);	// USART 0: /dev/ttyS0
	at32_map_usart(1, 1, 0);	// USART 1: /dev/ttyS1
	at32_map_usart(2, 2, 0);	// USART 2: /dev/ttyS2
	at32_map_usart(3, 3, 0);	// USART 3: /dev/ttyS3

	at32_setup_serial_console(CONFIG_BOARD_ICNOVA_OEM_DEFCON);
}

static int __init icnova_init(void)
{

	at32_add_device_usart(0);
	at32_add_device_usart(1);
	at32_add_device_usart(2);
	at32_add_device_usart(3);

	return 0;
}
postcore_initcall(icnova_init);
