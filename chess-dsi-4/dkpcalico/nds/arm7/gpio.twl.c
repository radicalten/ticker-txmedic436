// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/thread.h>
#include <calico/nds/arm7/gpio.h>

void gpioSetWlModule(GpioWlModule module)
{
	u16 wl_cur = REG_GPIO_WL;
	u16 wl_new = wl_cur;

	// Adjust register value based on the desired module
	switch (module) {
		default:
		case GpioWlModule_Atheros:
			wl_new &= ~GPIO_WL_MITSUMI;
			break;
		case GpioWlModule_Mitsumi:
			wl_new |= GPIO_WL_MITSUMI;
			break;
	}

	// Succeed early if the setting already matches
	if (wl_cur == wl_new) {
		return;
	}

	// Apply the new setting
	REG_GPIO_WL = wl_new;
	if (module == GpioWlModule_Mitsumi) {
		// 5ms delay needed after selecting Mitsumi
		threadSleep(5000);
	}
}
