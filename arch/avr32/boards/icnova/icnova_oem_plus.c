/*
 * init code specific for KaVo Chairside
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
#include <mach/portmux.h>
#include <mach/at32ap700x.h>

static int __init icnova_init2(void)
{
	//NCS4 & NCS5
	at32_select_periph(GPIO_PIOE_BASE, (1<<21)|(1<<22), GPIO_PERIPH_A, 0);
	at32_select_periph(GPIO_PIOA_BASE, (1<<31), GPIO_PERIPH_A, 0); //GCLK1

	return 0;
}
postcore_initcall(icnova_init2);
