/*
 * This just describes the host-controller on the full-featured ICNova.
 *
 * (C) 2008 by Benjamin Tietz <benjamin@micronet24.de>
 */

#ifndef __BOARD_ICNOVA_USB_H
#define __BOARD_ICNOVA_USB_H

#include <mach/at32ap700x.h>

#ifdef CONFIG_BOARD_ICNOVA_OEMPLUSBASED
#define USB_NCS		4
#define USB_MEM_START	0x04000000
#define USB_NRESET	27 // -1
#define USB_SUSPEND	GPIO_PIN_PE(18)
#define USB_IRQ		64
#else
#define USB_NCS		3
#define USB_MEM_START	0x0C000000
#define USB_NRESET	GPIO_PIN_PA(26)
#define USB_SUSPEND	GPIO_PIN_PA(24)
#define USB_IRQ		65
#endif
#define USB_MEM_SIZE	((1<<18)-1)
#define USB_DMA		7

#endif
