/*
 *
 * device driver for Conexant 2388x based TV cards
 * card-specific stuff.
 *
 * (c) 2003 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>

#include "cx88.h"

/* ------------------------------------------------------------------ */
/* board config info                                                  */

struct cx88_board cx88_boards[] = {
	[CX88_BOARD_UNKNOWN] = {
		.name		= "UNKNOWN/GENERIC",
		.tuner_type     = UNSET,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 0,
		},{
			.type   = CX88_VMUX_COMPOSITE2,
			.vmux   = 1,
		},{
			.type   = CX88_VMUX_COMPOSITE3,
			.vmux   = 2,
		},{
			.type   = CX88_VMUX_COMPOSITE4,
			.vmux   = 3,
		}},
	},
	[CX88_BOARD_HAUPPAUGE] = {
		.name		= "Hauppauge WinTV 34xxx models",
		.tuner_type     = UNSET,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xff00,  // internal decoder
		},{
			.type   = CX88_VMUX_DEBUG,
			.vmux   = 0,
			.gpio0  = 0xff01,  // mono from tuner chip
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0xff02,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0xff02,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0xff01,
		},
	},
	[CX88_BOARD_GDI] = {
		.name		= "GDI Black Gold",
		.tuner_type     = UNSET,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
		}},
	},
	[CX88_BOARD_PIXELVIEW] = {
		.name           = "PixelView",
		.tuner_type     = TUNER_PHILIPS_PAL,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xff00,  // internal decoder
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
		}},
		.radio = {
			 .type  = CX88_RADIO,
			 .gpio0 = 0xff10,
		 },
	},
	[CX88_BOARD_ATI_WONDER_PRO] = {
		.name           = "ATI TV Wonder Pro",
		.tuner_type     = TUNER_PHILIPS_4IN1,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_INTERCARRIER,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x03ff,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x03fe,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x03fe,
		}},
	},
	[CX88_BOARD_WINFAST2000XP_EXPERT] = {
		.name           = "Leadtek Winfast 2000XP Expert",
		.tuner_type     = TUNER_PHILIPS_4IN1,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0	= 0x00F5e700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5e700,
			.gpio3  = 0x02000000,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0	= 0x00F5c700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5c700,
			.gpio3  = 0x02000000,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0	= 0x00F5c700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5c700,
			.gpio3  = 0x02000000,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0	= 0x00F5d700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x00F5d700,
			.gpio3  = 0x02000000,
		},
	},
	[CX88_BOARD_AVERTV_STUDIO_303] = {
		.name           = "AverTV Studio 303 (M126)",
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio1  = 0x309f,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio1  = 0x305f,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio1  = 0x305f,
		}},
		.radio = {
			.type   = CX88_RADIO,
		},
	},
	[CX88_BOARD_MSI_TVANYWHERE_MASTER] = {
		// added gpio values thanks to Michal
		// values for PAL from DScaler
		.name           = "MSI TV-@nywhere Master",
		.tuner_type     = TUNER_MT2032,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf	= TDA9887_PRESENT | TDA9887_INTERCARRIER_NTSC,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x000040bf,
			.gpio1  = 0x000080c0,
			.gpio2  = 0x0000ff40,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x000040bf,
			.gpio1  = 0x000080c0,
			.gpio2  = 0x0000ff40,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x000040bf,
			.gpio1  = 0x000080c0,
			.gpio2  = 0x0000ff40,
		}},
		.radio = {
			 .type   = CX88_RADIO,
		},
	},
	[CX88_BOARD_WINFAST_DV2000] = {
		.name           = "Leadtek Winfast DV2000",
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x0035e700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x0035e700,
			.gpio3  = 0x02000000,
		},{

			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x0035c700,
			.gpio1  = 0x00003004,
			.gpio2  = 0x0035c700,
			.gpio3  = 0x02000000,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x0035c700,
			.gpio1  = 0x0035c700,
			.gpio2  = 0x02000000,
			.gpio3  = 0x02000000,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0x0035d700,
			.gpio1  = 0x00007004,
			.gpio2  = 0x0035d700,
			.gpio3  = 0x02000000,
		 },
	},
	[CX88_BOARD_LEADTEK_PVR2000] = {
		// gpio values for PAL version from regspy by DScaler
		.name           = "Leadtek PVR 2000",
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x0000bde2,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x0000bde6,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x0000bde6,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0x0000bd62,
		},
		.blackbird = 1,
	},
	[CX88_BOARD_IODATA_GVVCP3PCI] = {
		.name		= "IODATA GV-VCP3/PCI",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 0,
		},{
			.type   = CX88_VMUX_COMPOSITE2,
			.vmux   = 1,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
		}},
	},
	[CX88_BOARD_PROLINK_PLAYTVPVR] = {
		.name           = "Prolink PlayTV PVR",
		.tuner_type     = TUNER_PHILIPS_FM1236_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf	= TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xff00,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0xff03,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0xff03,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0xff00,
		},
	},
	[CX88_BOARD_ASUS_PVR_416] = {
		.name		= "ASUS PVR-416",
		.tuner_type     = TUNER_PHILIPS_FM1236_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x0000fde6,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x0000fde6, // 0x0000fda6 L,R RCA audio in?
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0x0000fde2,
		},
		.blackbird = 1,
	},
	[CX88_BOARD_MSI_TVANYWHERE] = {
		.name           = "MSI TV-@nywhere",
		.tuner_type     = TUNER_MT2032,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x00000fbf,
			.gpio2  = 0x0000fc08,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x00000fbf,
			.gpio2  = 0x0000fc68,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x00000fbf,
			.gpio2  = 0x0000fc68,
		}},
	},
	[CX88_BOARD_KWORLD_DVB_T] = {
		.name           = "KWorld/VStream XPert DVB-T",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x0700,
			.gpio2  = 0x0101,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x0700,
			.gpio2  = 0x0101,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_DVB_T1] = {
		.name           = "DViCO FusionHDTV DVB-T1",
		.tuner_type     = TUNER_ABSENT, /* No analog tuner */
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x000027df,
		 },{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x000027df,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_KWORLD_LTV883] = {
		.name           = "KWorld LTV883RF",
		.tuner_type     = TUNER_TNF_8831BGFF,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x07f8,
		},{
			.type   = CX88_VMUX_DEBUG,
			.vmux   = 0,
			.gpio0  = 0x07f9,  // mono from tuner chip
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x000007fa,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x000007fa,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0x000007f8,
		},
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_3_GOLD_Q] = {
		.name		= "DViCO FusionHDTV 3 Gold-Q",
		.tuner_type     = TUNER_MICROTUNE_4042FI5,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		/*
		   GPIO[0] resets DT3302 DTV receiver
		    0 - reset asserted
		    1 - normal operation
		   GPIO[1] mutes analog audio output connector
		    0 - enable selected source
		    1 - mute
		   GPIO[2] selects source for analog audio output connector
		    0 - analog audio input connector on tab
		    1 - analog DAC output from CX23881 chip
		   GPIO[3] selects RF input connector on tuner module
		    0 - RF connector labeled CABLE
		    1 - RF connector labeled ANT
		   GPIO[4] selects high RF for QAM256 mode
		    0 - normal RF
		    1 - high RF
		*/
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0	= 0x0f0d,
		},{
			.type   = CX88_VMUX_CABLE,
			.vmux   = 0,
			.gpio0	= 0x0f05,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0	= 0x0f00,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0	= 0x0f00,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_HAUPPAUGE_DVB_T1] = {
		.name           = "Hauppauge Nova-T DVB-T",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_DVB,
			.vmux   = 0,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_CONEXANT_DVB_T1] = {
		.name           = "Conexant DVB-T reference design",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_DVB,
			.vmux   = 0,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_PROVIDEO_PV259] = {
		.name		= "Provideo PV259",
		.tuner_type     = TUNER_PHILIPS_FQ1216ME,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
		}},
		.blackbird = 1,
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_PLUS] = {
		.name           = "DViCO FusionHDTV DVB-T Plus",
		.tuner_type     = TUNER_ABSENT, /* No analog tuner */
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x000027df,
		 },{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x000027df,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_DNTV_LIVE_DVB_T] = {
		.name		= "digitalnow DNTV Live! DVB-T",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input		= {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x00000700,
			.gpio2  = 0x00000101,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x00000700,
			.gpio2  = 0x00000101,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_PCHDTV_HD3000] = {
		.name           = "pcHDTV HD3000 HDTV",
		.tuner_type     = TUNER_THOMSON_DTT7610,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x00008484,
			.gpio1  = 0x00000000,
			.gpio2  = 0x00000000,
			.gpio3  = 0x00000000,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x00008400,
			.gpio1  = 0x00000000,
			.gpio2  = 0x00000000,
			.gpio3  = 0x00000000,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x00008400,
			.gpio1  = 0x00000000,
			.gpio2  = 0x00000000,
			.gpio3  = 0x00000000,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.vmux   = 2,
			.gpio0  = 0x00008400,
			.gpio1  = 0x00000000,
			.gpio2  = 0x00000000,
			.gpio3  = 0x00000000,
		},
		.dvb            = 1,
	},
	[CX88_BOARD_HAUPPAUGE_ROSLYN] = {
		// entry added by Kaustubh D. Bhalerao <bhalerao.1@osu.edu>
		// GPIO values obtained from regspy, courtesy Sean Covel
		.name           = "Hauppauge WinTV 28xxx (Roslyn) models",
		.tuner_type     = UNSET,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xed1a,
			.gpio2  = 0x00ff,
		},{
			.type   = CX88_VMUX_DEBUG,
			.vmux   = 0,
			.gpio0  = 0xff01,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0xff02,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0xed92,
			.gpio2  = 0x00ff,
		}},
		.radio = {
			 .type   = CX88_RADIO,
			 .gpio0  = 0xed96,
			 .gpio2  = 0x00ff,
		 },
		.blackbird = 1,
	},
	[CX88_BOARD_DIGITALLOGIC_MEC] = {
		.name           = "Digital-Logic MICROSPACE Entertainment Center (MEC)",
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x00009d80,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x00009d76,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x00009d76,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.gpio0  = 0x00009d00,
		},
		.blackbird = 1,
	},
	[CX88_BOARD_IODATA_GVBCTV7E] = {
		.name           = "IODATA GV/BCTV7E",
		.tuner_type     = TUNER_PHILIPS_FQ1286,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 1,
			.gpio1  = 0x0000e03f,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 2,
			.gpio1  = 0x0000e07f,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 3,
			.gpio1  = 0x0000e07f,
		}}
	},
	[CX88_BOARD_PIXELVIEW_PLAYTV_ULTRA_PRO] = {
		.name           = "PixelView PlayTV Ultra Pro (Stereo)",
		/* May be also TUNER_YMEC_TVF_5533MF for NTSC/M or PAL/M */
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xbf61,  /* internal decoder */
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0	= 0xbf63,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0	= 0xbf63,
		}},
		.radio = {
			 .type  = CX88_RADIO,
			 .gpio0 = 0xbf60,
		 },
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_3_GOLD_T] = {
		.name           = "DViCO FusionHDTV 3 Gold-T",
		.tuner_type     = TUNER_THOMSON_DTT761X,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x97ed,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x97e9,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x97e9,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_ADSTECH_DVB_T_PCI] = {
		.name           = "ADS Tech Instant TV DVB-T PCI",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x0700,
			.gpio2  = 0x0101,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x0700,
			.gpio2  = 0x0101,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_TERRATEC_CINERGY_1400_DVB_T1] = {
		.name           = "TerraTec Cinergy 1400 DVB-T",
		.tuner_type     = TUNER_ABSENT,
		.input          = {{
			.type   = CX88_VMUX_DVB,
			.vmux   = 0,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_5_GOLD] = {
		.name           = "DViCO FusionHDTV 5 Gold",
		.tuner_type     = TUNER_LG_TDVS_H062F,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x87fd,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x87f9,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x87f9,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_AVERMEDIA_ULTRATV_MC_550] = {
		.name           = "AverMedia UltraTV Media Center PCI 550",
		.tuner_type     = TUNER_PHILIPS_FM1236_MK3,
		.radio_type     = UNSET,
		.tuner_addr     = ADDR_UNSET,
		.radio_addr     = ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.blackbird      = 1,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 0,
			.gpio0  = 0x0000cd73,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 1,
			.gpio0  = 0x0000cd73,
		},{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 3,
			.gpio0  = 0x0000cdb3,
		}},
		.radio = {
			.type   = CX88_RADIO,
			.vmux   = 2,
			.gpio0  = 0x0000cdf3,
		},
	},
	[CX88_BOARD_KWORLD_VSTREAM_EXPERT_DVD] = {
		 /* Alexander Wold <awold@bigfoot.com> */
		 .name           = "Kworld V-Stream Xpert DVD",
		 .tuner_type     = UNSET,
		 .input          = {{
			 .type   = CX88_VMUX_COMPOSITE1,
			 .vmux   = 1,
			 .gpio0  = 0x03000000,
			 .gpio1  = 0x01000000,
			 .gpio2  = 0x02000000,
			 .gpio3  = 0x00100000,
		 },{
			 .type   = CX88_VMUX_SVIDEO,
			 .vmux   = 2,
			 .gpio0  = 0x03000000,
			 .gpio1  = 0x01000000,
			 .gpio2  = 0x02000000,
			 .gpio3  = 0x00100000,
		 }},
	},
	[CX88_BOARD_ATI_HDTVWONDER] = {
		.name           = "ATI HDTV Wonder",
		.tuner_type     = TUNER_PHILIPS_TUV1236D,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x00000ff7,
			.gpio1  = 0x000000ff,
			.gpio2  = 0x00000001,
			.gpio3  = 0x00000000,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x00000ffe,
			.gpio1  = 0x000000ff,
			.gpio2  = 0x00000001,
			.gpio3  = 0x00000000,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x00000ffe,
			.gpio1  = 0x000000ff,
			.gpio2  = 0x00000001,
			.gpio3  = 0x00000000,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_WINFAST_DTV1000] = {
		.name           = "WinFast DTV1000-T",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_DVB,
			.vmux   = 0,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_AVERTV_303] = {
		.name           = "AVerTV 303 (M126)",
		.tuner_type     = TUNER_PHILIPS_FM1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0x00ff,
			.gpio1  = 0xe09f,
			.gpio2  = 0x0010,
			.gpio3  = 0x0000,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x00ff,
			.gpio1  = 0xe05f,
			.gpio2  = 0x0010,
			.gpio3  = 0x0000,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x00ff,
			.gpio1  = 0xe05f,
			.gpio2  = 0x0010,
			.gpio3  = 0x0000,
		}},
	},
	[CX88_BOARD_HAUPPAUGE_NOVASPLUS_S1] = {
		.name		= "Hauppauge Nova-S-Plus DVB-S",
		.tuner_type	= TUNER_ABSENT,
		.radio_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input		= {{
			.type	= CX88_VMUX_DVB,
			.vmux	= 0,
		},{
			.type	= CX88_VMUX_COMPOSITE1,
			.vmux	= 1,
		},{
			.type	= CX88_VMUX_SVIDEO,
			.vmux	= 2,
		}},
		.dvb		= 1,
	},
	[CX88_BOARD_HAUPPAUGE_NOVASE2_S1] = {
		.name		= "Hauppauge Nova-SE2 DVB-S",
		.tuner_type	= TUNER_ABSENT,
		.radio_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input		= {{
			.type	= CX88_VMUX_DVB,
			.vmux	= 0,
		}},
		.dvb		= 1,
	},
	[CX88_BOARD_KWORLD_DVBS_100] = {
		.name		= "KWorld DVB-S 100",
		.tuner_type	= TUNER_ABSENT,
		.radio_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input		= {{
			.type	= CX88_VMUX_DVB,
			.vmux	= 0,
		},{
			.type	= CX88_VMUX_COMPOSITE1,
			.vmux	= 1,
		},{
			.type	= CX88_VMUX_SVIDEO,
			.vmux	= 2,
		}},
		.dvb		= 1,
	},
	[CX88_BOARD_HAUPPAUGE_HVR1100] = {
		.name		= "Hauppauge WinTV-HVR1100 DVB-T/Hybrid",
		.tuner_type     = TUNER_PHILIPS_FMD1216ME_MK3,
		.radio_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input		= {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
		},{
			.type	= CX88_VMUX_COMPOSITE1,
			.vmux	= 1,
		},{
			.type	= CX88_VMUX_SVIDEO,
			.vmux	= 2,
		}},
		/* fixme: Add radio support */
		.dvb		= 1,
	},
	[CX88_BOARD_HAUPPAUGE_HVR1100LP] = {
		.name		= "Hauppauge WinTV-HVR1100 DVB-T/Hybrid (Low Profile)",
		.tuner_type     = TUNER_PHILIPS_FMD1216ME_MK3,
		.radio_type	= UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT,
		.input		= {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
		},{
			.type	= CX88_VMUX_COMPOSITE1,
			.vmux	= 1,
		}},
		/* fixme: Add radio support */
		.dvb		= 1,
	},
	[CX88_BOARD_DNTV_LIVE_DVB_T_PRO] = {
		.name           = "digitalnow DNTV Live! DVB-T Pro",
		.tuner_type     = TUNER_PHILIPS_FMD1216ME_MK3,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.tda9887_conf   = TDA9887_PRESENT | TDA9887_PORT1_ACTIVE |
				  TDA9887_PORT2_ACTIVE,
		.input          = {{
			.type   = CX88_VMUX_TELEVISION,
			.vmux   = 0,
			.gpio0  = 0xf80808,
		},{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0	= 0xf80808,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0	= 0xf80808,
		}},
		.radio = {
			 .type  = CX88_RADIO,
			 .gpio0 = 0xf80808,
		},
		.dvb            = 1,
	},
	[CX88_BOARD_KWORLD_DVB_T_CX22702] = {
		/* Kworld V-stream Xpert DVB-T with Thomson tuner */
		/* DTT 7579 Conexant CX22702-19 Conexant CX2388x  */
		/* Manenti Marco <marco_manenti@colman.it> */
		.name           = "KWorld/VStream XPert DVB-T with cx22702",
		.tuner_type     = TUNER_ABSENT,
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x0700,
			.gpio2  = 0x0101,
		},{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x0700,
			.gpio2  = 0x0101,
		}},
		.dvb            = 1,
	},
	[CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_DUAL] = {
		.name           = "DViCO FusionHDTV DVB-T Dual Digital",
		.tuner_type     = TUNER_ABSENT, /* No analog tuner */
		.radio_type     = UNSET,
		.tuner_addr	= ADDR_UNSET,
		.radio_addr	= ADDR_UNSET,
		.input          = {{
			.type   = CX88_VMUX_COMPOSITE1,
			.vmux   = 1,
			.gpio0  = 0x000027df,
		 },{
			.type   = CX88_VMUX_SVIDEO,
			.vmux   = 2,
			.gpio0  = 0x000027df,
		}},
		.dvb            = 1,
	},

};
const unsigned int cx88_bcount = ARRAY_SIZE(cx88_boards);

/* ------------------------------------------------------------------ */
/* PCI subsystem IDs                                                  */

struct cx88_subid cx88_subids[] = {
	{
		.subvendor = 0x0070,
		.subdevice = 0x3400,
		.card      = CX88_BOARD_HAUPPAUGE,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x3401,
		.card      = CX88_BOARD_HAUPPAUGE,
	},{
		.subvendor = 0x14c7,
		.subdevice = 0x0106,
		.card      = CX88_BOARD_GDI,
	},{
		.subvendor = 0x14c7,
		.subdevice = 0x0107, /* with mpeg encoder */
		.card      = CX88_BOARD_GDI,
	},{
		.subvendor = PCI_VENDOR_ID_ATI,
		.subdevice = 0x00f8,
		.card      = CX88_BOARD_ATI_WONDER_PRO,
	},{
		.subvendor = 0x107d,
		.subdevice = 0x6611,
		.card      = CX88_BOARD_WINFAST2000XP_EXPERT,
	},{
		.subvendor = 0x107d,
		.subdevice = 0x6613,	/* NTSC */
		.card      = CX88_BOARD_WINFAST2000XP_EXPERT,
	},{
		.subvendor = 0x107d,
		.subdevice = 0x6620,
		.card      = CX88_BOARD_WINFAST_DV2000,
	},{
		.subvendor = 0x107d,
		.subdevice = 0x663b,
		.card      = CX88_BOARD_LEADTEK_PVR2000,
	},{
		.subvendor = 0x107d,
		.subdevice = 0x663C,
		.card      = CX88_BOARD_LEADTEK_PVR2000,
	},{
		.subvendor = 0x1461,
		.subdevice = 0x000b,
		.card      = CX88_BOARD_AVERTV_STUDIO_303,
	},{
		.subvendor = 0x1462,
		.subdevice = 0x8606,
		.card      = CX88_BOARD_MSI_TVANYWHERE_MASTER,
	},{
		.subvendor = 0x10fc,
		.subdevice = 0xd003,
		.card      = CX88_BOARD_IODATA_GVVCP3PCI,
	},{
		.subvendor = 0x1043,
		.subdevice = 0x4823,  /* with mpeg encoder */
		.card      = CX88_BOARD_ASUS_PVR_416,
	},{
		.subvendor = 0x17de,
		.subdevice = 0x08a6,
		.card      = CX88_BOARD_KWORLD_DVB_T,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xd810,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_3_GOLD_Q,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xd820,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_3_GOLD_T,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xdb00,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_DVB_T1,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9002,
		.card      = CX88_BOARD_HAUPPAUGE_DVB_T1,
	},{
		.subvendor = 0x14f1,
		.subdevice = 0x0187,
		.card      = CX88_BOARD_CONEXANT_DVB_T1,
	},{
		.subvendor = 0x1540,
		.subdevice = 0x2580,
		.card      = CX88_BOARD_PROVIDEO_PV259,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xdb10,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_PLUS,
	},{
		.subvendor = 0x1554,
		.subdevice = 0x4811,
		.card      = CX88_BOARD_PIXELVIEW,
	},{
		.subvendor = 0x7063,
		.subdevice = 0x3000, /* HD-3000 card */
		.card      = CX88_BOARD_PCHDTV_HD3000,
	},{
		.subvendor = 0x17de,
		.subdevice = 0xa8a6,
		.card      = CX88_BOARD_DNTV_LIVE_DVB_T,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x2801,
		.card      = CX88_BOARD_HAUPPAUGE_ROSLYN,
	},{
		.subvendor = 0x14f1,
		.subdevice = 0x0342,
		.card      = CX88_BOARD_DIGITALLOGIC_MEC,
	},{
		.subvendor = 0x10fc,
		.subdevice = 0xd035,
		.card      = CX88_BOARD_IODATA_GVBCTV7E,
	},{
		.subvendor = 0x1421,
		.subdevice = 0x0334,
		.card      = CX88_BOARD_ADSTECH_DVB_T_PCI,
	},{
		.subvendor = 0x153b,
		.subdevice = 0x1166,
		.card      = CX88_BOARD_TERRATEC_CINERGY_1400_DVB_T1,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xd500,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_5_GOLD,
	},{
		.subvendor = 0x1461,
		.subdevice = 0x8011,
		.card      = CX88_BOARD_AVERMEDIA_ULTRATV_MC_550,
	},{
		.subvendor = PCI_VENDOR_ID_ATI,
		.subdevice = 0xa101,
		.card      = CX88_BOARD_ATI_HDTVWONDER,
	},{
		.subvendor = 0x107d,
		.subdevice = 0x665f,
		.card      = CX88_BOARD_WINFAST_DTV1000,
	},{
		.subvendor = 0x1461,
		.subdevice = 0x000a,
		.card      = CX88_BOARD_AVERTV_303,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9200,
		.card      = CX88_BOARD_HAUPPAUGE_NOVASE2_S1,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9201,
		.card      = CX88_BOARD_HAUPPAUGE_NOVASPLUS_S1,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9202,
		.card      = CX88_BOARD_HAUPPAUGE_NOVASPLUS_S1,
	},{
		.subvendor = 0x17de,
		.subdevice = 0x08b2,
		.card      = CX88_BOARD_KWORLD_DVBS_100,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9400,
		.card      = CX88_BOARD_HAUPPAUGE_HVR1100,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9402,
		.card      = CX88_BOARD_HAUPPAUGE_HVR1100,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9800,
		.card      = CX88_BOARD_HAUPPAUGE_HVR1100LP,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9802,
		.card      = CX88_BOARD_HAUPPAUGE_HVR1100LP,
	},{
		.subvendor = 0x0070,
		.subdevice = 0x9001,
		.card      = CX88_BOARD_HAUPPAUGE_DVB_T1,
	},{
		.subvendor = 0x1822,
		.subdevice = 0x0025,
		.card      = CX88_BOARD_DNTV_LIVE_DVB_T_PRO,
	},{
		.subvendor = 0x17de,
		.subdevice = 0x08a1,
		.card      = CX88_BOARD_KWORLD_DVB_T_CX22702,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xdb50,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_DUAL,
	},{
		.subvendor = 0x18ac,
		.subdevice = 0xdb11,
		.card      = CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_PLUS,
		/* Re-branded DViCO: UltraView DVB-T Plus */
	},
};
const unsigned int cx88_idcount = ARRAY_SIZE(cx88_subids);

/* ----------------------------------------------------------------------- */
/* some leadtek specific stuff                                             */

static void __devinit leadtek_eeprom(struct cx88_core *core, u8 *eeprom_data)
{
	/* This is just for the "Winfast 2000XP Expert" board ATM; I don't have data on
	 * any others.
	 *
	 * Byte 0 is 1 on the NTSC board.
	 */

	if (eeprom_data[4] != 0x7d ||
	    eeprom_data[5] != 0x10 ||
	    eeprom_data[7] != 0x66) {
		printk(KERN_WARNING "%s: Leadtek eeprom invalid.\n",
		       core->name);
		return;
	}

	core->has_radio  = 1;
	core->tuner_type = (eeprom_data[6] == 0x13) ? 43 : 38;

	printk(KERN_INFO "%s: Leadtek Winfast 2000XP Expert config: "
	       "tuner=%d, eeprom[0]=0x%02x\n",
	       core->name, core->tuner_type, eeprom_data[0]);
}

static void hauppauge_eeprom(struct cx88_core *core, u8 *eeprom_data)
{
	struct tveeprom tv;

	tveeprom_hauppauge_analog(&core->i2c_client, &tv, eeprom_data);
	core->tuner_type = tv.tuner_type;
	core->tuner_formats = tv.tuner_formats;
	core->has_radio  = tv.has_radio;

	/* Make sure we support the board model */
	switch (tv.model)
	{
	case 28552: /* WinTV-PVR 'Roslyn' (No IR) */
	case 90002: /* Nova-T-PCI (9002) */
	case 92001: /* Nova-S-Plus (Video and IR) */
	case 92002: /* Nova-S-Plus (Video and IR) */
	case 90003: /* Nova-T-PCI (9002 No RF out) */
	case 90500: /* Nova-T-PCI (oem) */
	case 90501: /* Nova-T-PCI (oem/IR) */
	case 92000: /* Nova-SE2 (OEM, No Video or IR) */
	case 94009: /* WinTV-HVR1100 (Video and IR Retail) */
	case 94501: /* WinTV-HVR1100 (Video and IR OEM) */
	case 98559: /* WinTV-HVR1100LP (Video no IR, Retail - Low Profile) */
		/* known */
		break;
	default:
		printk("%s: warning: unknown hauppauge model #%d\n",
		       core->name, tv.model);
		break;
	}

	printk(KERN_INFO "%s: hauppauge eeprom: model=%d\n",
			core->name, tv.model);
}

/* ----------------------------------------------------------------------- */
/* some GDI (was: Modular Technology) specific stuff                       */

static struct {
	int  id;
	int  fm;
	char *name;
} gdi_tuner[] = {
	[ 0x01 ] = { .id   = TUNER_ABSENT,
		     .name = "NTSC_M" },
	[ 0x02 ] = { .id   = TUNER_ABSENT,
		     .name = "PAL_B" },
	[ 0x03 ] = { .id   = TUNER_ABSENT,
		     .name = "PAL_I" },
	[ 0x04 ] = { .id   = TUNER_ABSENT,
		     .name = "PAL_D" },
	[ 0x05 ] = { .id   = TUNER_ABSENT,
		     .name = "SECAM" },

	[ 0x10 ] = { .id   = TUNER_ABSENT,
		     .fm   = 1,
		     .name = "TEMIC_4049" },
	[ 0x11 ] = { .id   = TUNER_TEMIC_4136FY5,
		     .name = "TEMIC_4136" },
	[ 0x12 ] = { .id   = TUNER_ABSENT,
		     .name = "TEMIC_4146" },

	[ 0x20 ] = { .id   = TUNER_PHILIPS_FQ1216ME,
		     .fm   = 1,
		     .name = "PHILIPS_FQ1216_MK3" },
	[ 0x21 ] = { .id   = TUNER_ABSENT, .fm = 1,
		     .name = "PHILIPS_FQ1236_MK3" },
	[ 0x22 ] = { .id   = TUNER_ABSENT,
		     .name = "PHILIPS_FI1236_MK3" },
	[ 0x23 ] = { .id   = TUNER_ABSENT,
		     .name = "PHILIPS_FI1216_MK3" },
};

static void gdi_eeprom(struct cx88_core *core, u8 *eeprom_data)
{
	char *name = (eeprom_data[0x0d] < ARRAY_SIZE(gdi_tuner))
		? gdi_tuner[eeprom_data[0x0d]].name : NULL;

	printk(KERN_INFO "%s: GDI: tuner=%s\n", core->name,
	       name ? name : "unknown");
	if (NULL == name)
		return;
	core->tuner_type = gdi_tuner[eeprom_data[0x0d]].id;
	core->has_radio  = gdi_tuner[eeprom_data[0x0d]].fm;
}

/* ----------------------------------------------------------------------- */

void cx88_card_list(struct cx88_core *core, struct pci_dev *pci)
{
	int i;

	if (0 == pci->subsystem_vendor &&
	    0 == pci->subsystem_device) {
		printk("%s: Your board has no valid PCI Subsystem ID and thus can't\n"
		       "%s: be autodetected.  Please pass card=<n> insmod option to\n"
		       "%s: workaround that.  Redirect complaints to the vendor of\n"
		       "%s: the TV card.  Best regards,\n"
		       "%s:         -- tux\n",
		       core->name,core->name,core->name,core->name,core->name);
	} else {
		printk("%s: Your board isn't known (yet) to the driver.  You can\n"
		       "%s: try to pick one of the existing card configs via\n"
		       "%s: card=<n> insmod option.  Updating to the latest\n"
		       "%s: version might help as well.\n",
		       core->name,core->name,core->name,core->name);
	}
	printk("%s: Here is a list of valid choices for the card=<n> insmod option:\n",
	       core->name);
	for (i = 0; i < cx88_bcount; i++)
		printk("%s:    card=%d -> %s\n",
		       core->name, i, cx88_boards[i].name);
}

void cx88_card_setup(struct cx88_core *core)
{
	static u8 eeprom[256];

	if (0 == core->i2c_rc) {
		core->i2c_client.addr = 0xa0 >> 1;
		tveeprom_read(&core->i2c_client,eeprom,sizeof(eeprom));
	}

	switch (core->board) {
	case CX88_BOARD_HAUPPAUGE:
	case CX88_BOARD_HAUPPAUGE_ROSLYN:
		if (0 == core->i2c_rc)
			hauppauge_eeprom(core,eeprom+8);
		break;
	case CX88_BOARD_GDI:
		if (0 == core->i2c_rc)
			gdi_eeprom(core,eeprom);
		break;
	case CX88_BOARD_WINFAST2000XP_EXPERT:
		if (0 == core->i2c_rc)
			leadtek_eeprom(core,eeprom);
		break;
	case CX88_BOARD_HAUPPAUGE_NOVASPLUS_S1:
	case CX88_BOARD_HAUPPAUGE_NOVASE2_S1:
	case CX88_BOARD_HAUPPAUGE_DVB_T1:
	case CX88_BOARD_HAUPPAUGE_HVR1100:
	case CX88_BOARD_HAUPPAUGE_HVR1100LP:
		if (0 == core->i2c_rc)
			hauppauge_eeprom(core,eeprom);
		break;
	case CX88_BOARD_KWORLD_DVBS_100:
		cx_write(MO_GP0_IO, 0x000007f8);
		cx_write(MO_GP1_IO, 0x00000001);
		break;
	case CX88_BOARD_DVICO_FUSIONHDTV_DVB_T1:
	case CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_PLUS:
	case CX88_BOARD_DVICO_FUSIONHDTV_DVB_T_DUAL:
		/* GPIO0:0 is hooked to mt352 reset pin */
		cx_set(MO_GP0_IO, 0x00000101);
		cx_clear(MO_GP0_IO, 0x00000001);
		msleep(1);
		cx_set(MO_GP0_IO, 0x00000101);
		break;
	case CX88_BOARD_KWORLD_DVB_T:
	case CX88_BOARD_DNTV_LIVE_DVB_T:
		cx_set(MO_GP0_IO, 0x00000707);
		cx_set(MO_GP2_IO, 0x00000101);
		cx_clear(MO_GP2_IO, 0x00000001);
		msleep(1);
		cx_clear(MO_GP0_IO, 0x00000007);
		cx_set(MO_GP2_IO, 0x00000101);
		break;
	case CX88_BOARD_DNTV_LIVE_DVB_T_PRO:
		cx_write(MO_GP0_IO, 0x00080808);
		break;
	case CX88_BOARD_ATI_HDTVWONDER:
		if (0 == core->i2c_rc) {
			/* enable tuner */
			int i;
			u8 buffer [] = { 0x10,0x12,0x13,0x04,0x16,0x00,0x14,0x04,0x017,0x00 };
			core->i2c_client.addr = 0x0a;

			for (i = 0; i < 5; i++)
				if (2 != i2c_master_send(&core->i2c_client,&buffer[i*2],2))
					printk(KERN_WARNING "%s: Unable to enable tuner(%i).\n",
						core->name, i);
		}
		break;
	}
	if (cx88_boards[core->board].radio.type == CX88_RADIO)
		core->has_radio = 1;
}

/* ------------------------------------------------------------------ */

EXPORT_SYMBOL(cx88_boards);
EXPORT_SYMBOL(cx88_bcount);
EXPORT_SYMBOL(cx88_subids);
EXPORT_SYMBOL(cx88_idcount);
EXPORT_SYMBOL(cx88_card_list);
EXPORT_SYMBOL(cx88_card_setup);

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 * kate: eol "unix"; indent-width 3; remove-trailing-space on; replace-trailing-space-save on; tab-width 8; replace-tabs off; space-indent off; mixed-indent off
 */
