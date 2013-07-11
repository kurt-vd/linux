
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <mach/portmux.h>
#include <mach/board.h>
#include <mach/gpio.h>

#define DEFLED(PWM, NAME) \
	{ \
		.name = NAME, \
		.gpio = PWM, \
	}

static struct gpio_led icnova_ledpwm[] = {
};

static struct gpio_led_platform_data icnova_ledpwm_data = {
	.num_leds = ARRAY_SIZE(icnova_ledpwm),
	.leds = icnova_ledpwm,
};

static struct platform_device icnova_ledpwm_dev = {
	.name = "leds-atmel-pwm",
	.dev = {
		.platform_data = &icnova_ledpwm_data,
	},
};

static int __init icnova_setup_leds(void)
{
	int pwmmask = 0, j;

	printk("ICnova: %lu PWM Leds\n", ARRAY_SIZE(icnova_ledpwm));
	for (j = 0; j < ARRAY_SIZE(icnova_ledpwm); ++j) {
		pwmmask |= 1 << icnova_ledpwm[j].gpio;
	}
	at32_add_device_pwm(pwmmask); //All four channels
	platform_device_register(&icnova_ledpwm_dev);

	return 0;
}

arch_initcall(icnova_setup_leds);
