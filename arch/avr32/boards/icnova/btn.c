/*
 * Configure the GPIO-Buttons to act as input (using the gpio-keyboard driver)
 *
 * licensed under the terms of the GPL
 * (C) 2007 Benjamin Tietz <benjamin@micronet24.de>
 */

#include <linux/types.h>
#include <linux/gpio_keys.h>
#include <linux/input.h>
#include <linux/platform_device.h>

#include <mach/at32ap700x.h>
#include <mach/portmux.h>
#include <mach/gpio.h>

#define btn(PORT, PIN, KEYCODE, NAME) \
	{ \
		.active_low = 1, \
		.gpio = GPIO_PIN_P##PORT(PIN), \
		.type = EV_KEY, \
		.code = KEYCODE, \
		.desc = NAME, \
	}
static struct gpio_keys_button icnova_buttons_btn[] = {
	{
		.gpio = GPIO_PIN_PA(31),
		.code = BTN_0,
		.desc = "SW1",
		.active_low = 1,
		.type = EV_KEY,
	},
#ifdef CONFIG_BOARD_LINUMATIC
	btn(D, 3, 1, "in01"),
	btn(D, 2, 2, "in02"),
	btn(D, 1, 3, "in03"),
	btn(D, 0, 4, "in04"),
	btn(D, 4, 5, "in05"),
	btn(D, 5, 6, "in06"),
	btn(D, 6, 7, "in07"),
	btn(D, 7, 8, "in08"),
	btn(D, 8, 9, "in09"),
	btn(D, 9, 10, "in10"),
	btn(D, 10, 11, "in11"),
	btn(D, 11, 12, "in12"),
	btn(D, 12, 13, "in13"),
	btn(D, 13, 14, "in14"),
	btn(D, 14, 15, "in15"),
	btn(D, 15, 16, "in16"),
	btn(A, 12, 17, "in17"),
	btn(D, 16, 18, "in18"),
	btn(D, 17, 19, "in19"),
	btn(C, 19, 20, "in20"),
	btn(C, 20, 21, "in21"),
	btn(C, 21, 22, "in22"),
	btn(C, 22, 23, "in23"),
	btn(C, 23, 24, "in24"),
	btn(C, 24, 25, "in25"),
	btn(C, 25, 26, "in26"),
	btn(C, 26, 27, "in27"),
	btn(C, 27, 28, "in28"),
	btn(C, 28, 29, "in29"),
	btn(C, 29, 30, "in30"),
	btn(C, 30, 31, "in31"),
	btn(C, 31, 32, "in32"),
#endif
};

static struct gpio_keys_platform_data icnova_buttons_data = {
	.buttons = icnova_buttons_btn,
	.nbuttons = ARRAY_SIZE(icnova_buttons_btn),
};

static struct platform_device icnova_buttons = {
	.name = "gpio-keys",
	.id = -1,
	.num_resources = 0,
	.dev = {
		.platform_data = &icnova_buttons_data,
	},
};

static int __init icnova_setup_buttons(void) {
	int i;
	for(i=0;i<icnova_buttons_data.nbuttons;i++)
		at32_select_gpio(icnova_buttons_data.buttons[i].gpio,
				AT32_GPIOF_DEGLITCH);

	platform_device_register(&icnova_buttons);
	return 0;
}

arch_initcall(icnova_setup_buttons);
