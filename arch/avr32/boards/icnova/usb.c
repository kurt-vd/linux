/*
 * This just sets up the USB-Host controller
 *
 * (C) 2008 by Benjamin Tietz <benjamin@micronet24.de>
 * licensed under the terms of the GPLv2
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/usb/isp1760.h>
#include <mach/at32ap700x.h>
#include <mach/board.h>
#include <mach/portmux.h>
#include <mach/gpio.h>
#include <mach/smc.h>
#include "usb.h"

#define USB_NAME "isp176x"

static struct resource icnova_ehci_res[] = {
	{
		.start = USB_MEM_START,
		.end = USB_MEM_START + USB_MEM_SIZE,
		.flags = IORESOURCE_MEM,
	},
	{
		.start = USB_IRQ,
		.end = USB_IRQ,
		.flags = IORESOURCE_IRQ|
			IORESOURCE_IRQ_SHAREABLE|IORESOURCE_IRQ_LOWLEVEL,
	},
	/*{
		.start = USB_DMA,
		.end = USB_DMA,
		.flags = IORESOURCE_DMA,
	},*/
};

static struct isp1760_platform_data icnova_ehci_platform = {
	.buswidth = 32,
	.flags =
		ISP1760_FLAG_PORT1_DIS|
//		ISP1760_FLAG_ANALOG_OC|
		ISP1760_FLAG_ISP1761,
};

// static u64 icnova_usb_dma_mask = DMA_32BIT_MASK;

static struct platform_device icnova_usbh = {
	.name = USB_NAME,
	.id = 0,
	.dev = {
		.platform_data = &icnova_ehci_platform,
		//.dma_mask = &icnova_usb_dma_mask,
		//.coherent_dma_mask = DMA_32_BIT_MASK,
	},
	.resource = icnova_ehci_res,
	.num_resources = ARRAY_SIZE(icnova_ehci_res),
};

static struct __initdata smc_timing icnova_usb_timing = {
	.ncs_read_setup = 0,
	.nrd_setup = 0,
	.ncs_write_setup = 0,
	.nwe_setup = 0,
	.ncs_read_pulse = 22,
	.nrd_pulse = 22,
	.ncs_write_pulse = 18,
	.nwe_pulse = 17,
	.read_cycle = 50,
	.write_cycle = 50,
	.ncs_read_recover = 1,
	.nrd_recover = 1,
	.ncs_write_recover = 60,
	.nwe_recover = 2,
};

static struct __initdata smc_config icnova_usb_smc_config = {
	.byte_write = 0,
	.bus_width = 4,
	.tdf_mode = 0,
	.tdf_cycles = 1,
	.nwe_controlled = 0,
	.nrd_controlled = 1,
	.nwait_mode = 0,
};

int icnova_add_usb(void) {
	int ret;

	pr_warning("registring " USB_NAME "\n");
	// Setup PINS
	//at32_select_periph(GPIO_PIN_PB(26), GPIO_PERIPH_B, 0);
	//at32_select_periph(GPIO_PIN_PB(24), GPIO_PERIPH_A, 0);
	gpio_request(USB_SUSPEND, USB_NAME ".suspend");
	gpio_request(USB_NRESET, USB_NAME ".reset");
	gpio_direction_input(USB_SUSPEND);
	gpio_direction_output(USB_NRESET, 0);
	udelay(100); // Make sure to perform a reset
	gpio_set_value(USB_NRESET, 1);
//	gpio_export(USB_NRESET,0);
//	gpio_export(USB_SUSPEND,1);

	// Setup SMC
	smc_set_timing(&icnova_usb_smc_config,&icnova_usb_timing);
	if(icnova_usb_smc_config.ncs_write_pulse <=
			icnova_usb_smc_config.nwe_pulse) {
		icnova_usb_smc_config.ncs_write_pulse =
			icnova_usb_smc_config.nwe_pulse + 1;
	}
	smc_set_configuration(USB_NCS,&icnova_usb_smc_config);

	// Register device
	ret = platform_device_register(&icnova_usbh);
	if(ret) goto err_dev_add;
	return 0;

err_dev_add:
	pr_warning(USB_NAME " failed.\n");
	return ret;
}
device_initcall(icnova_add_usb);
