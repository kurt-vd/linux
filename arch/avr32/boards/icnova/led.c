
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <mach/portmux.h>
#include <mach/at32ap700x.h>
#include <mach/gpio.h>

static struct gpio_led icnova_led[] = {
#if defined(CONFIG_BOARD_ICNOVA_BASE)
	{
		.name = "led1:green",
		.gpio = GPIO_PIN_PA(23),
		.default_trigger = "heartbeat",
	},
	{
		.name = "led2:green",
		.gpio = GPIO_PIN_PA(24),
	},
	{
		.name = "led3:green",
		.gpio = GPIO_PIN_PA(25),
	},
	{
		.name = "led4:green",
		.gpio = GPIO_PIN_PA(26),
	},
	{
		.name = "led5:green",
		.gpio = GPIO_PIN_PA(27),
	},
	{
		.name = "led6:green",
		.gpio = GPIO_PIN_PA(28),
	},
	{
		.name = "led7:green",
		.gpio = GPIO_PIN_PA(29),
	},
	{
		.name = "led8:green",
		.gpio = GPIO_PIN_PA(30),
	},
	{
		.name = "pwrled:red",
		.gpio = GPIO_PIN_PA(22),
		.active_low = 1,
	},
#endif
#ifdef CONFIG_BOARD_ICNOVA_FULL
	{
		.name = "pwrled:red",
		.gpio = GPIO_PIN_PA(25),
		.active_low = 1,
	},
	{
		.name = "led1:green",
		.gpio = GPIO_PIN_PC(18),
		.default_trigger = "heartbeat",
	},
#endif
#ifdef CONFIG_BOARD_ICNOVA_ADB1000
	{
		.name = "pwrled:red",
		.gpio = GPIO_PIN_PA(27),
		.active_low = 1,
	},
	{
		.name = "led1:green",
		.gpio = GPIO_PIN_PE(16),
		.default_trigger = "heartbeat",
	},
	{
		.name = "led2:green",
		.gpio = GPIO_PIN_PE(17),
	},
	{
		.name = "led3:green",
		.gpio = GPIO_PIN_PE(24),
	},
	{
		.name = "led4:green",
		.gpio = GPIO_PIN_PE(25),
	},
#endif
#ifdef CONFIG_BOARD_ICNOVA_ADB1001
	{
		.name = "led1:green",
		.gpio = GPIO_PIN_PE(16),
		.default_trigger = "heartbeat",
	},
#endif
};

static struct gpio_led_platform_data icnova_led_data = {
	.num_leds = ARRAY_SIZE(icnova_led),
	.leds = icnova_led,
};

static struct platform_device icnova_led_dev = {
	.name = "leds-gpio",
	.dev = {
		.platform_data = &icnova_led_data,
	},
};

static int __init icnova_setup_leds(void) {

	printk("ICnova: %lu Leds\n", ARRAY_SIZE(icnova_led));
	platform_device_register(&icnova_led_dev);

	return 0;
}

arch_initcall(icnova_setup_leds);
