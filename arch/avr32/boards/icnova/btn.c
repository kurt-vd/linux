/*
 * Configure the GPIO-Buttons to act as input (using the gpio-keyboard driver)
 *
 * licensed under the terms of the GPL
 * (C) 2007 Benjamin Tietz <benjamin@micronet24.de>
 */

#include <linux/gpio_keys.h>
#include <linux/input.h>
#include <linux/platform_device.h>

#include <mach/at32ap700x.h>
#include <mach/portmux.h>
#include <mach/gpio.h>

static struct gpio_keys_button icnova_buttons_btn[] = {
	{
		.gpio = GPIO_PIN_PA(31),
		.code = BTN_0,
		.desc = "Button 0",
		.active_low = 1,
		.type = EV_KEY,
	},
};

static struct gpio_keys_platform_data icnova_buttons_data = {
	.buttons = icnova_buttons_btn,
	.nbuttons = 1,
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
