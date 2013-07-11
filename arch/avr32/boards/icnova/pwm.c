/*
 * This just puts the selected PWMs into their appropriate state
 *
 * (C) 2009 by Benjamin Tietz <benjamin.tietz@in-circuit.de>
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <mach/init.h>
#include <mach/board.h>
#include <mach/portmux.h>
#include <mach/at32ap700x.h>

static struct platform_device icnova_pwmdev = {
	.name = "atmel-pwm-hwmon",
	.id = 0,
};

static int icnova_pwm(void) {
	char pwmmask = 0;
#ifdef CONFIG_BOARD_ICNOVA_PWM0
	pwmmask |= (1<<0);
#endif
#ifdef CONFIG_BOARD_ICNOVA_PWM1
	pwmmask |= (1<<1);
#endif
#ifdef CONFIG_BOARD_ICNOVA_PWM2
	pwmmask |= (1<<2);
#endif
#ifdef CONFIG_BOARD_ICNOVA_PWM3
	pwmmask |= (1<<3);
#endif
	at32_add_device_pwm(pwmmask); //All four channels
	platform_device_register(&icnova_pwmdev);
	return 0;
}
device_initcall(icnova_pwm);
