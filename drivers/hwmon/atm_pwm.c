/*
 * Copyright (C) 2009 by Benjamin Tietz <benjamin.tietz@in-circuit.de>
 * based on the atmel-pwm-bl.c
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */
#define DEBUG
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/atmel_pwm.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/sysfs.h>

#define ATMEL_PWM_NCHN	4
#define ATMEL_PWM_MAX	1000

struct atmel_pwm_hwmon {
	unsigned int		pwm_channel;
	struct hwmon_device	*hwdev;
	struct platform_device	*pdev;
	struct pwm_channel	pwmc[ATMEL_PWM_NCHN];
	struct device		dev;
};

static ssize_t atmel_pwm_hwmon_set_duty(struct device *dev,
		struct device_attribute *devattr, const char *buf, size_t count)
{
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct atmel_pwm_hwmon *pwmhw = platform_get_drvdata(pdev);
	unsigned int pwm_duty = simple_strtoul(buf, NULL, 10);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct pwm_channel *pwmc = &(pwmhw->pwmc[attr->index]);

	if(pwm_duty > ATMEL_PWM_MAX)
		pwm_duty = ATMEL_PWM_MAX;
	pwm_channel_writel(pwmc, PWM_CUPD, pwm_duty);

	return count;
}

static ssize_t atmel_pwm_hwmon_get_duty(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct atmel_pwm_hwmon *pwmhw = platform_get_drvdata(pdev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct pwm_channel *pwmc = &(pwmhw->pwmc[attr->index]);

	return sprintf(buf, "%d\n", pwm_channel_readl(pwmc, PWM_CDTY));
}

static ssize_t atmel_pwm_hwmon_set_freq(struct device *dev,
		struct device_attribute *devattr, const char *buf, size_t count)
{
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct atmel_pwm_hwmon *pwmhw = platform_get_drvdata(pdev);
	int pwm_freq = simple_strtoul(buf, NULL, 10);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct pwm_channel *pwmc = &(pwmhw->pwmc[attr->index]);
	unsigned long pwm_rate = pwmc->mck;
	unsigned long prescale;


	pwm_channel_disable(pwmc);
	if(!pwm_freq)
		return count;

	prescale = DIV_ROUND_UP(pwm_rate, (pwm_freq * ATMEL_PWM_MAX)) -1;
	/*
	 * Prescale must be power of two and maximum 0xf in size because of
	 * hardware limit. PWM speed will be:
	 *	PWM module clock speed / (2 ^ prescale).
	 */
	prescale = fls(prescale);
	if (prescale > 0xa)
		prescale = 0xa;

	pwm_channel_writel(pwmc, PWM_CPRD, ATMEL_PWM_MAX);
	pwm_channel_writel(pwmc, PWM_CMR, prescale);
	pwm_channel_enable(pwmc);

	return count;
}

static ssize_t atmel_pwm_hwmon_get_freq(struct device *dev,
		struct device_attribute *devattr, char *buf)
{
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct atmel_pwm_hwmon *pwmhw = platform_get_drvdata(pdev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct pwm_channel *pwmc = &(pwmhw->pwmc[attr->index]);
	unsigned long pwm_freq = pwm_channel_readl(pwmc, PWM_CMR);
	pwm_freq = 1<<pwm_freq;
	pwm_freq = pwmc->mck / pwm_freq / ATMEL_PWM_MAX;

	return sprintf(buf, "%lu\n", pwm_freq);
}

static SENSOR_DEVICE_ATTR(pwm0_freq, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_freq, atmel_pwm_hwmon_set_freq, 0);
static SENSOR_DEVICE_ATTR(pwm0, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_duty, atmel_pwm_hwmon_set_duty, 0);
static SENSOR_DEVICE_ATTR(pwm1_freq, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_freq, atmel_pwm_hwmon_set_freq, 1);
static SENSOR_DEVICE_ATTR(pwm1, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_duty, atmel_pwm_hwmon_set_duty, 1);
static SENSOR_DEVICE_ATTR(pwm2_freq, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_freq, atmel_pwm_hwmon_set_freq, 2);
static SENSOR_DEVICE_ATTR(pwm2, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_duty, atmel_pwm_hwmon_set_duty, 2);
static SENSOR_DEVICE_ATTR(pwm3_freq, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_freq, atmel_pwm_hwmon_set_freq, 3);
static SENSOR_DEVICE_ATTR(pwm3, S_IWUSR | S_IRUGO,
		atmel_pwm_hwmon_get_duty, atmel_pwm_hwmon_set_duty, 3);

static struct attribute *atmel_pwm_hwmon_attributes[] = {
	&sensor_dev_attr_pwm0_freq.dev_attr.attr,
	&sensor_dev_attr_pwm0.dev_attr.attr,
	NULL,
	&sensor_dev_attr_pwm1_freq.dev_attr.attr,
	&sensor_dev_attr_pwm1.dev_attr.attr,
	NULL,
	&sensor_dev_attr_pwm2_freq.dev_attr.attr,
	&sensor_dev_attr_pwm2.dev_attr.attr,
	NULL,
	&sensor_dev_attr_pwm3_freq.dev_attr.attr,
	&sensor_dev_attr_pwm3.dev_attr.attr,
	NULL,
};

static const struct attribute_group atmel_pwm_hwmon_group[] = {
	{
		.attrs = &(atmel_pwm_hwmon_attributes[0]),
	},
	{
		.attrs = &(atmel_pwm_hwmon_attributes[3]),
	},
	{
		.attrs = &(atmel_pwm_hwmon_attributes[6]),
	},
	{
		.attrs = &(atmel_pwm_hwmon_attributes[9]),
	},
};

static int atmel_pwm_hwmon_probe(struct platform_device *pdev)
{
	struct atmel_pwm_hwmon *pwmhw;
	int retval;
	int i;

	pwmhw = kzalloc(sizeof(struct atmel_pwm_hwmon), GFP_KERNEL);
	if (!pwmhw)
		return -ENOMEM;

	pwmhw->pdev = pdev;

	for(i=0; i< ATMEL_PWM_NCHN; i++) {
		retval = pwm_channel_alloc(i, &pwmhw->pwmc[i]);
		if(retval) continue;

		retval = sysfs_create_group(&pdev->dev.kobj,
				&(atmel_pwm_hwmon_group[i]));
		if(retval)
			goto err_free_pwm;
	}

	pwmhw->hwdev = hwmon_device_register(&pdev->dev);
	if(IS_ERR(pwmhw->hwdev)) {
		dev_dbg(&pdev->dev, "Can't register hwmon-device\n");
		retval = PTR_ERR(pwmhw->hwdev);
		goto err_hwmon;
	}

	platform_set_drvdata(pdev, pwmhw);

	return 0;

//err_free_hwmon_dev:
	platform_set_drvdata(pdev, NULL);
	hwmon_device_unregister(pwmhw->hwdev);
err_hwmon:
	i = ATMEL_PWM_NCHN;
err_free_pwm:
	for(i--;i>=0;i--) {
		sysfs_remove_group(&pdev->dev.kobj,
				&(atmel_pwm_hwmon_group[i]));
		pwm_channel_free(&pwmhw->pwmc[i]);
	}
//err_free_mem:
	kfree(pwmhw);
	return retval;
}

static int __exit atmel_pwm_hwmon_remove(struct platform_device *pdev)
{
	struct atmel_pwm_hwmon *pwmhw = platform_get_drvdata(pdev);
	int i;

	platform_set_drvdata(pdev, NULL);
	hwmon_device_unregister(pwmhw->hwdev);
	for(i=0;i<ATMEL_PWM_NCHN;i++)  {
		sysfs_remove_group(&pdev->dev.kobj,
				&(atmel_pwm_hwmon_group[i]));
		pwm_channel_disable(&pwmhw->pwmc[i]);
		pwm_channel_free(&pwmhw->pwmc[i]);
	}
	kfree(pwmhw);
	return 0;
}

static struct platform_driver atmel_pwm_hwmon_driver = {
	.driver = {
		.name = "atmel-pwm-hwmon",
	},
	/* REVISIT add suspend() and resume() */
	.remove = __exit_p(atmel_pwm_hwmon_remove),
};

static int __init atmel_pwm_hwmon_init(void)
{
	return platform_driver_probe(&atmel_pwm_hwmon_driver, atmel_pwm_hwmon_probe);
}
module_init(atmel_pwm_hwmon_init);

static void __exit atmel_pwm_hwmon_exit(void)
{
	platform_driver_unregister(&atmel_pwm_hwmon_driver);
}
module_exit(atmel_pwm_hwmon_exit);

MODULE_AUTHOR("Benjamin Tietz <benjamin.tietz@in-circuit.de>");
MODULE_DESCRIPTION("Atmel PWM hwmon driver");
MODULE_LICENSE("GPL");
