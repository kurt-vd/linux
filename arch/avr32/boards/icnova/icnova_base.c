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
#ifdef CONFIG_BOARD_ICNOVA_BASE_CP2102
	at32_map_usart(0, 1, 0);	// USART 0: /dev/ttyS1, IF-to-USB-UART_Bridge
	at32_map_usart(1, 0, 0);	// USART 1: /dev/ttyS0, CP2102
#else
	at32_map_usart(0, 0, 0);	// USART 0: /dev/ttyS0, IF-to-USB-UART_Bridge
#endif

	at32_setup_serial_console(0);
}

static int __init icnova_init(void)
{

	at32_add_device_usart(0);
#ifdef CONFIG_BOARD_ICNOVA_BASE_CP2102
	at32_add_device_usart(1);
#endif

	return 0;
}
postcore_initcall(icnova_init);
