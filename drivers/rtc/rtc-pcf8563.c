/*
 * An I2C driver for the Philips PCF8563 RTC
 * Copyright 2005-06 Tower Technologies
 *
 * Author: Alessandro Zummo <a.zummo@towertech.it>
 * Maintainers: http://www.nslu2-linux.org/
 *
 * based on the other drivers in this same directory.
 *
 * http://www.semiconductors.philips.com/acrobat/datasheets/PCF8563-04.pdf
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>

#define DRV_VERSION "0.4.3"
#define SHOW_REGS

#define PCF8563_REG_ST1		0x00 /* status */
#define PCF8563_REG_ST2		0x01

#define PCF8563_REG_SC		0x02 /* datetime */
#define PCF8563_REG_MN		0x03
#define PCF8563_REG_HR		0x04
#define PCF8563_REG_DM		0x05
#define PCF8563_REG_DW		0x06
#define PCF8563_REG_MO		0x07
#define PCF8563_REG_YR		0x08

#define PCF8563_REG_AMN		0x09 /* alarm */
#define PCF8563_REG_AHR		0x0A
#define PCF8563_REG_ADM		0x0B
#define PCF8563_REG_ADW		0x0C

#define PCF8563_REG_CLKO	0x0D /* clock out */
#define PCF8563_REG_TMRC	0x0E /* timer control */
#define PCF8563_REG_TMR		0x0F /* timer */

#define PCF8563_NREG		0x10

#define PCF8563_SC_LV		0x80 /* low voltage */
#define PCF8563_MO_C		0x80 /* century */
#define PCF8563_ST2_TIE		0x01
#define PCF8563_ST2_AIE		0x02
#define PCF8563_ST2_TF		0x04
#define PCF8563_ST2_AF		0x08
#define PCF8563_ST2_TI_TP	0x10
#define PCF8563_AEN		0x80 /* per-item alarm disable */

static struct i2c_driver pcf8563_driver;

struct pcf8563 {
	struct rtc_device *rtc;
	/*
	 * The meaning of MO_C bit varies by the chip type.
	 * From PCF8563 datasheet: this bit is toggled when the years
	 * register overflows from 99 to 00
	 *   0 indicates the century is 20xx
	 *   1 indicates the century is 19xx
	 * From RTC8564 datasheet: this bit indicates change of
	 * century. When the year digit data overflows from 99 to 00,
	 * this bit is set. By presetting it to 0 while still in the
	 * 20th century, it will be set in year 2000, ...
	 * There seems no reliable way to know how the system use this
	 * bit.  So let's do it heuristically, assuming we are live in
	 * 1970...2069.
	 */
	int c_polarity;	/* 0: MO_C=1 means 19xx, otherwise MO_C=1 means 20xx */
	int voltage_low; /* incicates if a low_voltage was detected */
};

/*
 * In the routines that deal directly with the pcf8563 hardware, we use
 * rtc_time -- month 0-11, hour 0-23, yr = calendar year-epoch.
 */
static int pcf8563_get_datetime(struct i2c_client *client, struct rtc_time *tm)
{
	struct pcf8563 *pcf8563 = i2c_get_clientdata(client);
	unsigned char buf[13] = { PCF8563_REG_ST1 };

	struct i2c_msg msgs[] = {
		{/* setup read ptr */
			.addr = client->addr,
			.len = 1,
			.buf = buf
		},
		{/* read status + date */
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 13,
			.buf = buf
		},
	};

	/* read registers */
	if ((i2c_transfer(client->adapter, msgs, 2)) != 2) {
		dev_err(&client->dev, "%s: read error\n", __func__);
		return -EIO;
	}

	if (buf[PCF8563_REG_SC] & PCF8563_SC_LV) {
		pcf8563->voltage_low = 1;
		dev_info(&client->dev,
			"low voltage detected, date/time is not reliable.\n");
	}

	dev_dbg(&client->dev,
		"%s: raw data is st1=%02x, st2=%02x, sec=%02x, min=%02x, hr=%02x, "
		"mday=%02x, wday=%02x, mon=%02x, year=%02x\n",
		__func__,
		buf[0], buf[1], buf[2], buf[3],
		buf[4], buf[5], buf[6], buf[7],
		buf[8]);


	tm->tm_sec = bcd2bin(buf[PCF8563_REG_SC] & 0x7F);
	tm->tm_min = bcd2bin(buf[PCF8563_REG_MN] & 0x7F);
	tm->tm_hour = bcd2bin(buf[PCF8563_REG_HR] & 0x3F); /* rtc hr 0-23 */
	tm->tm_mday = bcd2bin(buf[PCF8563_REG_DM] & 0x3F);
	tm->tm_wday = buf[PCF8563_REG_DW] & 0x07;
	tm->tm_mon = bcd2bin(buf[PCF8563_REG_MO] & 0x1F) - 1; /* rtc mn 1-12 */
	tm->tm_year = bcd2bin(buf[PCF8563_REG_YR]);
	if (tm->tm_year < 70)
		tm->tm_year += 100;	/* assume we are in 1970...2069 */
	/* detect the polarity heuristically. see note above. */
	pcf8563->c_polarity = (buf[PCF8563_REG_MO] & PCF8563_MO_C) ?
		(tm->tm_year >= 100) : (tm->tm_year < 100);

	dev_dbg(&client->dev, "%s: tm is secs=%d, mins=%d, hours=%d, "
		"mday=%d, mon=%d, year=%d, wday=%d\n",
		__func__,
		tm->tm_sec, tm->tm_min, tm->tm_hour,
		tm->tm_mday, tm->tm_mon, tm->tm_year, tm->tm_wday);

	/* the clock can give out invalid datetime, but we cannot return
	 * -EINVAL otherwise hwclock will refuse to set the time on bootup.
	 */
	if (rtc_valid_tm(tm) < 0)
		dev_err(&client->dev, "retrieved date/time is not valid.\n");

	return 0;
}

static int pcf8563_set_datetime(struct i2c_client *client, struct rtc_time *tm)
{
	struct pcf8563 *pcf8563 = i2c_get_clientdata(client);
	int i, err;
	unsigned char buf[9];

	dev_dbg(&client->dev, "%s: secs=%d, mins=%d, hours=%d, "
		"mday=%d, mon=%d, year=%d, wday=%d\n",
		__func__,
		tm->tm_sec, tm->tm_min, tm->tm_hour,
		tm->tm_mday, tm->tm_mon, tm->tm_year, tm->tm_wday);

	/* hours, minutes and seconds */
	buf[PCF8563_REG_SC] = bin2bcd(tm->tm_sec);
	buf[PCF8563_REG_MN] = bin2bcd(tm->tm_min);
	buf[PCF8563_REG_HR] = bin2bcd(tm->tm_hour);

	buf[PCF8563_REG_DM] = bin2bcd(tm->tm_mday);

	/* month, 1 - 12 */
	buf[PCF8563_REG_MO] = bin2bcd(tm->tm_mon + 1);

	/* year and century */
	buf[PCF8563_REG_YR] = bin2bcd(tm->tm_year % 100);
	if (pcf8563->c_polarity ? (tm->tm_year >= 100) : (tm->tm_year < 100))
		buf[PCF8563_REG_MO] |= PCF8563_MO_C;

	buf[PCF8563_REG_DW] = tm->tm_wday & 0x07;

	/* write register's data */
	for (i = 0; i < 7; i++) {
		unsigned char data[2] = { PCF8563_REG_SC + i,
						buf[PCF8563_REG_SC + i] };

		err = i2c_master_send(client, data, sizeof(data));
		if (err != sizeof(data)) {
			dev_err(&client->dev,
				"%s: err=%d addr=%02x, data=%02x\n",
				__func__, err, data[0], data[1]);
			return -EIO;
		}
	};

	return 0;
}

#ifdef CONFIG_RTC_INTF_DEV
static int pcf8563_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	struct pcf8563 *pcf8563 = i2c_get_clientdata(to_i2c_client(dev));
	struct rtc_time tm;

	switch (cmd) {
	case RTC_VL_READ:
		if (pcf8563->voltage_low)
			dev_info(dev, "low voltage detected, date/time is not reliable.\n");

		if (copy_to_user((void __user *)arg, &pcf8563->voltage_low,
					sizeof(int)))
			return -EFAULT;
		return 0;
	case RTC_VL_CLR:
		/*
		 * Clear the VL bit in the seconds register in case
		 * the time has not been set already (which would
		 * have cleared it). This does not really matter
		 * because of the cached voltage_low value but do it
		 * anyway for consistency.
		 */
		if (pcf8563_get_datetime(to_i2c_client(dev), &tm))
			pcf8563_set_datetime(to_i2c_client(dev), &tm);

		/* Clear the cached value. */
		pcf8563->voltage_low = 0;

		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}
#else
#define pcf8563_rtc_ioctl NULL
#endif

static int pcf8563_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	return pcf8563_get_datetime(to_i2c_client(dev), tm);
}

static int pcf8563_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	return pcf8563_set_datetime(to_i2c_client(dev), tm);
}

/*
 * Utility to modify ST2 register that controls
 * the sources and state of INT pin.
 * returns the original content of ST2.
 */
static int pcf8563_getset_st2(struct device *dev, int mask, int value)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;
	uint8_t dat[2] = { PCF8563_REG_ST2, }, st2, orig_st2;
	struct i2c_msg rdmsgs[] = {
		{ i2c->addr, 0, 1, dat, },
		{ i2c->addr, I2C_M_RD, 1, dat+1, },
	}, wrmsgs[] = {
		{ i2c->addr, 0, 2, dat, },
	};

	ret = i2c_transfer(i2c->adapter, rdmsgs, ARRAY_SIZE(rdmsgs));
	if (ret < 0)
		return (ret < 0) ? ret : -EIO;
	orig_st2 = dat[1];

	if (!mask)
		/* nothing to change */
		return orig_st2;

	st2 = (orig_st2 & ~mask) | (value & mask);
	/* fix status bits: write 1 (means: leave) when not in mask */
	st2 |= (PCF8563_ST2_TF | PCF8563_ST2_AF) & ~mask;
	/* mask out 3 unused (msb) bits */
	st2 &= 0x1f;

	dat[1] = st2;
	ret = i2c_transfer(i2c->adapter, wrmsgs, ARRAY_SIZE(wrmsgs));
	if (ret < 0)
		return (ret < 0) ? ret : -EIO;
	return orig_st2;
}

static int pcf8563_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;
	uint8_t addr = 0, dat[PCF8563_NREG];
	struct i2c_msg msgs[] = {
		{ i2c->addr, 0, 1, &addr, },
		{ i2c->addr, I2C_M_RD, PCF8563_NREG, dat, },
	};

	ret = i2c_transfer(i2c->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < ARRAY_SIZE(msgs))
		return (ret < 0) ? ret : -EIO;

	alrm->time.tm_sec = 0;
	alrm->time.tm_min = -1;
	alrm->time.tm_hour = -1;
	alrm->time.tm_wday = -1;
	alrm->time.tm_mday = -1;
	alrm->time.tm_mon = -1;
	alrm->time.tm_year = -1;

	if (bcd2bin(!(dat[PCF8563_REG_AMN] & PCF8563_AEN)))
		alrm->time.tm_min = bcd2bin(dat[PCF8563_REG_AMN] & 0x7f);
	if (bcd2bin(!(dat[PCF8563_REG_AHR] & PCF8563_AEN)))
		alrm->time.tm_hour = bcd2bin(dat[PCF8563_REG_AHR] & 0x3f);
	if (bcd2bin(!(dat[PCF8563_REG_ADM] & PCF8563_AEN)))
		alrm->time.tm_mday = bcd2bin(dat[PCF8563_REG_ADM] & 0x3f);
	if (bcd2bin(!(dat[PCF8563_REG_ADW] & PCF8563_AEN)))
		alrm->time.tm_mday = bcd2bin(dat[PCF8563_REG_ADW] & 0x03);
	alrm->enabled = (dat[PCF8563_REG_ST2] & PCF8563_ST2_AIE) ? 1 : 0;
	alrm->pending = (dat[PCF8563_REG_ST2] & PCF8563_ST2_AF) ? 1 : 0;
	return 0;
}

static inline int mkalrmreg(int value, int inval)
{
	return (value > inval) ? PCF8563_AEN : bin2bcd(value);
}

static int pcf8563_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;
	uint8_t dat[PCF8563_NREG] = {
		[PCF8563_REG_AMN-1] = PCF8563_REG_AMN,
	};
	struct i2c_msg msgs = { i2c->addr, 0, 5, dat + PCF8563_REG_AMN -1, };

	/* clear AIE */
	ret = pcf8563_getset_st2(dev, PCF8563_ST2_AIE, 0);
	if (ret < 0)
		return ret;
	/* write values */
	dat[PCF8563_REG_AMN] = mkalrmreg(alrm->time.tm_min, 60);
	dat[PCF8563_REG_AHR] = mkalrmreg(alrm->time.tm_hour, 24);
	dat[PCF8563_REG_ADM] = mkalrmreg(alrm->time.tm_mday, 31);
	/*
	 * Disable weekday alarm, day-of-month may schedule an alarm
	 * that is >1week in the future
	 */
	dat[PCF8563_REG_ADW] = PCF8563_AEN;

	ret = i2c_transfer(i2c->adapter, &msgs, 1);
	if (ret < 1)
		return (ret < 0) ? ret : -EIO;
	/* set AIE, and clear AF */
	ret = pcf8563_getset_st2(dev, PCF8563_ST2_AIE | PCF8563_ST2_AF,
			PCF8563_ST2_AIE);
	if (ret < 0)
		return ret;
	return 0;
}

static const struct rtc_class_ops pcf8563_rtc_ops = {
	.ioctl		= pcf8563_rtc_ioctl,
	.read_time	= pcf8563_rtc_read_time,
	.set_time	= pcf8563_rtc_set_time,
	.read_alarm	= pcf8563_read_alarm,
	.set_alarm	= pcf8563_set_alarm,
};

/* SYSFS */
static ssize_t pcf8563_sysfs_show_INT(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	char *str;
	int ret;

	/* only fetch ST2 */
	ret = pcf8563_getset_st2(dev, 0, 0);
	if (ret < 0)
		return ret;

	str = buf;
	str += sprintf(str, "%s", (ret & PCF8563_ST2_TI_TP) ? "PULSE" : "INTERVAL");
	if (ret & PCF8563_ST2_TIE)
		str += sprintf(str, " TIE");
	if (ret & PCF8563_ST2_AIE)
		str += sprintf(str, " AIE");
	if (ret & PCF8563_ST2_TF)
		str += sprintf(str, " TF");
	if (ret & PCF8563_ST2_AF)
		str += sprintf(str, " AF");
	str += sprintf(str, "\n");
	return str - buf;
}
static DEVICE_ATTR(INT, S_IRUGO, pcf8563_sysfs_show_INT, NULL);

#ifdef SHOW_REGS
static ssize_t pcf8563_sysfs_show_regs(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	char *str;
	int ret, j;
	uint8_t addr = 0, dat[PCF8563_NREG];
	struct i2c_msg msgs[] = {
		{ i2c->addr, 0, 1, &addr, },
		{ i2c->addr, I2C_M_RD, PCF8563_NREG, dat, },
	};
	static const char *const regnames[] = {
		"st1", "st2",
		"v/sec", "min", "hour", "day", "wday", "c_mon", "year",
		"a-min", "a-hour", "a-day", "a-wday",
		"clkout",
		"t-ctrl", "timer",
	};

	ret = i2c_transfer(i2c->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < ARRAY_SIZE(msgs))
		return (ret < 0) ? ret : -EIO;

	str = buf;
	for (j = 0; j < ARRAY_SIZE(dat); ++j)
		str += sprintf(str, "%s\t%02x\n", regnames[j], dat[j]);
	return str - buf;
}
static DEVICE_ATTR(regs, S_IRUGO, pcf8563_sysfs_show_regs, NULL);
#endif

static struct attribute *attrs[] = {
	&dev_attr_INT.attr,
#ifdef SHOW_REGS
	&dev_attr_regs.attr,
#endif
	NULL,
};
static struct attribute_group attr_group = {
	.attrs = attrs,
};

/*
 * Disable TIE & AIE on boot.
 * Leave TF & AF untouched, so you could read the flags later
 * in order to detect a Timer or Alarm wakeup
 */
static int pcf8563_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct pcf8563 *pcf8563;

	int err = 0, ret;

	dev_dbg(&client->dev, "%s\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	pcf8563 = kzalloc(sizeof(struct pcf8563), GFP_KERNEL);
	if (!pcf8563)
		return -ENOMEM;

	dev_info(&client->dev, "chip found, driver version " DRV_VERSION "\n");

	i2c_set_clientdata(client, pcf8563);

	/*
	 * Fix /INT on boot, leave flags, but disable xIE,
	 * The idea of xIE is to wake me up, and I just did
	 */
	ret = pcf8563_getset_st2(&client->dev,
			PCF8563_ST2_AIE | PCF8563_ST2_TIE, 0);
	if (ret < 0)
		dev_warn(&client->dev, "get ST2 failed\n");

	/* add sysfs */
	if (sysfs_create_group(&client->dev.kobj, &attr_group) < 0)
		goto fail_sysfs;

	pcf8563->rtc = rtc_device_register(pcf8563_driver.driver.name,
				&client->dev, &pcf8563_rtc_ops, THIS_MODULE);

	if (IS_ERR(pcf8563->rtc)) {
		err = PTR_ERR(pcf8563->rtc);
		goto exit_kfree;
	}

	return 0;

exit_kfree:
	sysfs_remove_group(&client->dev.kobj, &attr_group);

fail_sysfs:
	i2c_set_clientdata(client, NULL);
	kfree(pcf8563);

	return err;
}

static int pcf8563_remove(struct i2c_client *client)
{
	struct pcf8563 *pcf8563 = i2c_get_clientdata(client);

	if (pcf8563->rtc)
		rtc_device_unregister(pcf8563->rtc);

	sysfs_remove_group(&client->dev.kobj, &attr_group);
	i2c_set_clientdata(client, NULL);
	kfree(pcf8563);

	return 0;
}

static const struct i2c_device_id pcf8563_id[] = {
	{ "pcf8563", 0 },
	{ "rtc8564", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pcf8563_id);

#ifdef CONFIG_OF
static const struct of_device_id pcf8563_of_match[] = {
	{ .compatible = "nxp,pcf8563" },
	{}
};
MODULE_DEVICE_TABLE(of, pcf8563_of_match);
#endif

static struct i2c_driver pcf8563_driver = {
	.driver		= {
		.name	= "rtc-pcf8563",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(pcf8563_of_match),
	},
	.probe		= pcf8563_probe,
	.remove		= pcf8563_remove,
	.id_table	= pcf8563_id,
};

module_i2c_driver(pcf8563_driver);

MODULE_AUTHOR("Alessandro Zummo <a.zummo@towertech.it>");
MODULE_DESCRIPTION("Philips PCF8563/Epson RTC8564 RTC driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
