/*
 * Code for initializeing the VGA-Port of ICnova
 *
 * Copyright (C) 2007 Benjamin Tietz, In-Circuit
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/screen_info.h>

#include <video/atmel_lcdc.h>

#include <mach/at32ap700x.h>
#include <mach/board.h>
#include <asm/setup.h>

#include <mach/gpio.h>

#ifdef CONFIG_BOARD_ICNOVA_SINGLE_MODE_TFT
# define VGA_SINGLE_MODE
# undef VGA_MULTI_MODE
#else
# define VGA_MULTI_MODE
#endif

static struct fb_videomode icnova_modes[] = {
#ifdef CONFIG_BOARD_ICNOVA_VGA_ET050000DH6
	{
		.refresh	= 50,
		.xres		= 800,
		.yres		= 480,
		.pixclock	= KHZ2PICOS(33260),
		.left_margin	= 178,
		.right_margin	= 38,
		.upper_margin	= 35,
		.lower_margin	= 8,
		.hsync_len	= 40,
		.vsync_len	= 2,
//		.sync		= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED,
		.flag		= FB_MODE_IS_VESA,
	},
#undef VGA_SINGLE_MODE
#undef VGA_MULTI_MODE
#endif
#ifdef CONFIG_BOARD_ICNOVA_VGA_ET035009DH6
	{
		.refresh	= 50,
		.xres		= 320,
		.yres		= 240,
		.pixclock	= KHZ2PICOS(6500),
		.left_margin	= 34,
		.right_margin	= 20,
		.upper_margin	= 9,
		.lower_margin	= 4,
		.hsync_len	= 34,
		.vsync_len	= 9,
		.sync		= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED,
		.flag		= FB_MODE_IS_VESA,
	},
#undef VGA_SINGLE_MODE
#undef VGA_MULTI_MODE
#endif

#ifdef VGA_SINGLE_MODE
	{
		.refresh	= CONFIG_BOARD_ICNOVA_VGA_VHZ,
		.xres		= CONFIG_BOARD_ICNOVA_VGA_XRES,
		.yres		= CONFIG_BOARD_ICNOVA_VGA_YRES,
		.pixclock	= KHZ2PICOS(CONFIG_BOARD_ICNOVA_VGA_PIXCLK),
		.left_margin	= CONFIG_BOARD_ICNOVA_VGA_LMARG,
		.right_margin	= CONFIG_BOARD_ICNOVA_VGA_RMARG,
		.upper_margin	= CONFIG_BOARD_ICNOVA_VGA_TMARG,
		.lower_margin	= CONFIG_BOARD_ICNOVA_VGA_BMARG,
		.hsync_len	= CONFIG_BOARD_ICNOVA_VGA_HSYNC,
		.vsync_len	= CONFIG_BOARD_ICNOVA_VGA_VSYNC,
		.sync		=
#ifdef CONFIG_BOARD_ICNOVA_VGA_SYNCH
			FB_SYNC_HOR_HIGH_ACT |
#endif
#ifdef CONFIG_BOARD_ICNOVA_VGA_SYNCV
			FB_SYNC_VERT_HIGH_ACT |
#endif
#ifdef CONFIG_BOARD_ICNOVA_VGA_SYNCE
			FB_SYNC_EXT |
#endif
#ifdef CONFIG_BOARD_ICNOVA_VGA_SYNCC
			FB_SYNC_COMP_HIGH_ACT |
#endif
#ifdef CONFIG_BOARD_ICNOVA_VGA_SYNCB
			FB_SYNC_BROADCAST |
#endif
#ifdef CONFIG_BOARD_ICNOVA_VGA_SYNCG
			FB_SYNC_ON_GREEN |
#endif
			0,
		.vmode		=
#ifdef CONFIG_BOARD_ICNOVA_VGA_INTERLACED
			FB_VMODE_INTERLACED,
#else
			FB_VMODE_NONINTERLACED,
#endif
		.flag		=
#ifdef CONFIG_BOARD_ICNOVA_VGA_VESA
			FB_MODE_IS_VESA |
#endif
			0,
	},
#undef VGA_MULTI_MODE
#endif

#ifdef VGA_MULTI_MODE
	// Taken from vesa_modes
//	{
	  /* 320x240 @ 60 Hz, 31.5 kHz hsync, 4:3 aspect ratio */
//	  NULL, 60, 320, 240, 79440, 16, 16, 16, 5, 48, 1,
//	  0, FB_VMODE_DOUBLE
//	},
//	{
	  /* 320x240 @ 72 Hz, 36.5 kHz hsync */
//	  NULL, 72, 320, 240, 63492, 16, 16, 16, 4, 48, 2,
//	  0, FB_VMODE_DOUBLE
//	},
	/* 0 640x350-85 VESA */
//	{ "640x350-85", 85, 640, 350, 31746,  96, 32, 60, 32, 64, 3,
//	  FB_SYNC_HOR_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA},
	/* 1 640x400-85 VESA */
//	{ "640x400-85", 85, 640, 400, 31746,  96, 32, 41, 01, 64, 3,
//	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 2 720x400-85 VESA */
//	{ "720x400-85", 85, 721, 400, 28169, 108, 36, 42, 01, 72, 3,
//	  FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 3 640x480-60 VESA */
//	{ "640x480-60", 60, 640, 480, 39682,  58, 6, 33, 10, 96, 2,
//	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 4 640x480-72 VESA */
//	{ "640x480-72", 72, 640, 480, 31746, 128, 24, 29, 9, 40, 2,
//	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 5 640x480-75 VESA */
//	{ "640x480-75", 75, 640, 480, 31746, 120, 16, 16, 01, 64, 3,
//	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 6 640x480-85 VESA */
//	{ "640x480-85", 85, 640, 480, 27777, 80, 56, 25, 01, 56, 3,
//	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 7 800x600-56 VESA */
	{ "800x600-56", 56, 800, 600, 27777, 128, 24, 22, 01, 72, 2,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 8 800x600-60 VESA */
	{ "800x600-60", 60, 800, 600, 25000, 88, 40, 23, 01, 128, 4,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 9 800x600-72 VESA */
	{ "800x600-72", 72, 800, 600, 20000, 64, 56, 23, 37, 120, 6,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 10 800x600-75 VESA */
	{ "800x600-75", 75, 800, 600, 20202, 160, 16, 21, 01, 80, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 11 800x600-85 VESA */
	{ "800x600-85", 85, 800, 600, 17761, 152, 32, 27, 01, 64, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
        /* 12 1024x768i-43 VESA */
//	{ "1024x768i-43", 43, 1024, 768, 22271, 56, 8, 41, 0, 176, 8,
//	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
//	  FB_VMODE_INTERLACED, FB_MODE_IS_VESA },
	/* 13 1024x768-60 VESA */
	{ "1024x768-60", 60, 1024, 768, 15384, 160, 24, 29, 3, 136, 6,
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 14 1024x768-70 VESA */
	{ "1024x768-70", 70, 1024, 768, 13333, 144, 24, 29, 3, 136, 6,
	  0, FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 15 1024x768-75 VESA */
	{ "1024x768-75", 75, 1024, 768, 12690, 176, 16, 28, 1, 96, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
	/* 16 1024x768-85 VESA */
	{ "1024x768-85", 85, 1024, 768, 10582, 208, 48, 36, 1, 96, 3,
	  FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	  FB_VMODE_NONINTERLACED, FB_MODE_IS_VESA },
#endif
};

static struct fb_monspecs __initdata icnova_default_monspecs = {
	.manufacturer		= "VESA",
	.monitor		= "Generic VGA",
	.modedb			= icnova_modes,
	.modedb_len		= ARRAY_SIZE(icnova_modes),
	.hfmin			= 10000,
	.hfmax			= 30000,
	.vfmin			= 50,
	.vfmax			= 80,
	.dclkmax		= 30000000, // 30MHz
};

struct atmel_lcdfb_info __initdata icnova_lcdc_data = {
#ifdef CONFIG_BOARD_ICNOVA_VGA_ET035009DH6
	.default_bpp		= 24,
#else
	.default_bpp		= 16,
#endif
	.default_dmacon		= ATMEL_LCDC_DMAEN | ATMEL_LCDC_DMA2DEN,
	.default_lcdcon2	= (ATMEL_LCDC_DISTYPE_TFT
#ifdef CONFIG_BOARD_ICNOVA_CLK_INVERTED
				   | ATMEL_LCDC_INVCLK
#endif
#ifdef CONFIG_BOARD_ICNOVA_CLK_ALWAYSACTIVE
				   | ATMEL_LCDC_CLKMOD_ALWAYSACTIVE
#endif
#ifdef CONFIG_BOARD_ICNOVA_DVAL_INVERTED
				   | ATMEL_LCDC_INVDVAL
#endif
				   | ATMEL_LCDC_MEMOR_BIG),
#ifdef CONFIG_BOARD_ICNOVA_LCDCONISBL
	.lcdcon_is_backlight	= true,
#endif
	.default_monspecs	= &icnova_default_monspecs,
	.guard_time		= 2,
	.lcd_wiring_mode	= ATMEL_LCDC_WIRING_BGR,
};

static int icnova_add_vga(void) {
	gpio_request(GPIO_PIN_PC(24), "lcdc.mode");
	gpio_direction_output(GPIO_PIN_PC(24), 1);
	gpio_request(GPIO_PIN_PC(25), "lcdc.pwr");
#ifdef CONFIG_BOARD_ICNOVA_VGA_ET050000DH6
	gpio_direction_output(GPIO_PIN_PC(25), 1);
	gpio_request(GPIO_PIN_PC(23), "lcdc.enb");
	gpio_direction_output(GPIO_PIN_PC(23), 1);
#else
	gpio_direction_output(GPIO_PIN_PC(25), 0);
#endif
//#define GRAPHIC_ET070
#ifdef GRAPHIC_ET070
	gpio_direction_output(GPIO_PIN_PD(0), 0);
	gpio_direction_output(GPIO_PIN_PD(1), 1);
	gpio_direction_output(GPIO_PIN_PD(8), 1);
#endif
	at32_add_device_lcdc(0, &icnova_lcdc_data, fbmem_start, fbmem_size,
			(ATMEL_LCDC_PRI_24BIT|ATMEL_LCDC_PRI_CONTROL)
			& ~ATMEL_LCDC(PC, MODE)
			& ~ATMEL_LCDC(PC, PWR)
#ifdef CONFIG_BOARD_ICNOVA_VGA_ET050000DH6
			& ~ATMEL_LCDC(PC, DVAL)
#endif
#ifdef GRAPHIC_ET070
			& ~(ATMEL_LCDC(PD, DATA6)|ATMEL_LCDC(PD, DATA7)
				|ATMEL_LCDC(PD, DATA14))
			& ~(ATMEL_LCDC(PC, HSYNC)|ATMEL_LCDC(PC, VSYNC))
#endif
			);
	return 0;
}

device_initcall(icnova_add_vga);
