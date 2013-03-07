/*
 * Init-code for the IrDA-Interface
 *
 * licensed under the terms of the GPL
 * (C) 2007 Benjamin Tietz <benjamin@micronet24.de>
 */
#ifndef __ARCH_AVR32_BOARD_ICNOVA_IRDA_H
#define __ARCH_AVR32_BOARD_ICNOVA_IRDA_H

#include <linux/atmel_serial.h>

void __init icnova_setup_irda(struct platform_device *pdev);

#endif
