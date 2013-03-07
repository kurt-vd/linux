/*
 * Init-code for the IrDA-Interface
 *
 * licensed under the terms of the GPL
 * (C) 2007 Benjamin Tietz <benjamin@micronet24.de>
 */
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <asm/io.h>
#include <linux/atmel_serial.h>

void __init icnova_setup_irda(struct platform_device *pdev) {
	if(pdev->resource[0].flags != IORESOURCE_MEM)
		return;

	__raw_writel(
			(__raw_readl( pdev->resource[0].start + ATMEL_US_MR)
			 & ~(ATMEL_US_USMODE | ATMEL_US_FILTER))
			 | ATMEL_US_USMODE_IRDA | ATMEL_US_FILTER,
			pdev->resource[0].start + ATMEL_US_MR
		    );
	__raw_writeb(0x01,pdev->resource[0].start + ATMEL_US_IF);
}
