/*
 * This is the board setup code for the big ICnovas
 *
 * (C) 2008 by Benjamin Tietz <benjamin.tietz@in-circuit.de>
 */

#include <linux/init.h>
#include <mach/init.h>
#include <mach/board.h>
#include "irda.h"

void __init setup_board(void)
{
	at32_map_usart(1, 0);	// USART 1: /dev/ttyS0, DB/IF-to-USB_UART-BRIDGE
	at32_map_usart(0, 1);	// USART 0: /dev/ttyS1, RF-Connector
	at32_map_usart(3, 2);	// USART 3: /dev/ttyS2, IR
	// USART2 -> Ext IRQs

	at32_setup_serial_console(0);
}

static int __init icnova_setup_uart(void) {
	at32_add_device_usart(0);
	at32_add_device_usart(1);
	icnova_setup_irda(at32_add_device_usart(2));
	return 0;
}

postcore_initcall(icnova_setup_uart);
