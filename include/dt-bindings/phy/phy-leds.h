/*
 * Copyright (C) 2016 Hauke Mehrtens <hauke@xxxxxxxxxx>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef _DT_BINDINGS_PHY_LEDS
#define _DT_BINDINGS_PHY_LEDS

#define PHY_LED_OFF		(1 << 0)	/* is off */
#define PHY_LED_LINK10		(1 << 1)	/* link is 10MBit/s */
#define PHY_LED_LINK100		(1 << 2)	/* link is 100MBit/s */
#define PHY_LED_LINK1000	(1 << 3)	/* link is 1000MBit/s */
#define PHY_LED_PDOWN		(1 << 4)	/* link is powered down */
#define PHY_LED_EEE		(1 << 5)	/* link is in EEE mode */
#define PHY_LED_ANEG		(1 << 6)	/* auto negotiation is running */
#define PHY_LED_ABIST		(1 << 7)	/* analog self testing is running */
#define PHY_LED_CDIAG		(1 << 8)	/* cable diagnostics is running */
#define PHY_LED_COPPER		(1 << 9)	/* copper interface detected */
#define PHY_LED_FIBER		(1 << 10)	/* fiber interface detected */
#define PHY_LED_TXACT		(1 << 11)	/* Transmit activity */
#define PHY_LED_RXACT		(1 << 12)	/* Receive activity */
#define PHY_LED_COL		(1 << 13)	/* Collision */

#endif /* _DT_BINDINGS_PHY_LEDS */
