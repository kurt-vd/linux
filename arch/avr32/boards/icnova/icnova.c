/*
 * ICNOVA-specific init code
 *
 * (C) 2008 by Benjamin Tietz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/bootmem.h>
#include <linux/linkage.h>
#include <linux/kernel.h>

#include <mach/at32ap700x.h>
#include <mach/board.h>
#include <mach/portmux.h>
#include <mach/at32ap700x.h>

/*
 * Oscillator rates. Board specific
 */
unsigned long at32_board_osc_rates[3] = {
        [0] = 32768,    /* 32.768 kHz on RTC osc */
        [1] = 20000000, /* 20 MHz on osc0 */
        [2] = 12000000, /* 12 MHz on osc1 */
};

/* Initialized by bootloader-specific startup code. */
struct tag *bootloader_tags __initdata;

/*
 * ICNOVA uses 32-bit SDRAM interface. Reserve the
 * SDRAM-specific pins so that nobody messes with them.
 */
inline static void icnova_reserve_pins(void) {
	at32_reserve_pin(GPIO_PIOE_BASE, ATMEL_EBI_PE_DATA_ALL);
}

static int __init icnova_init(void)
{
	icnova_reserve_pins();

	at32_add_device_usba(0, NULL);

	return 0;
}

postcore_initcall(icnova_init);
