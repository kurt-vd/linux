/*
 * Initialization-code for AC97 on ICnova
 *
 * (C) 2009 by Benjamin Tietz
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <mach/at32ap700x.h>
#include <mach/board.h>
#include <sound/atmel-ac97c.h>

static struct ac97c_platform_data ac97c_data = {
	.reset_pin = CONFIG_BOARD_ICNOVA_AC97_RESET,
};

static int __init ac97_init(void) {
	at32_add_device_ac97c(0, &ac97c_data, AC97C_BOTH);
	return 0;
}

postcore_initcall(ac97_init);
