// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/arm/cache.h>
#include <calico/nds/pxi.h>
#include <calico/nds/scfg.h>
#include <calico/nds/pm.h>
#include "../pxi/pm.h"

unsigned pmGetBatteryState(void)
{
	u32 msg = pxiPmMakeMsg(PxiPmMsg_GetBatteryState, 0);
	return pxiSendAndReceive(PxiChannel_Power, msg);
}

void pmSetPowerLed(PmLedMode mode)
{
	u32 msg = pxiPmMakeMsg(PxiPmMsg_SetPowerLed, mode);
	pxiSendAndReceive(PxiChannel_Power, msg);
}

bool pmReadNvram(void* data, u32 addr, u32 len)
{
	u32 msg = pxiPmMakeMsg(PxiPmMsg_ReadNvram, 0);
	u32 args[] = {
		(u32)data,
		addr,
		len,
	};

	armDCacheFlush(data, len);
	return pxiSendWithDataAndReceive(PxiChannel_Power, msg, args, 3);
}

void pmMicSetAmp(bool enable, unsigned gain)
{
	if (gain > PmMicGain_Max) {
		gain = PmMicGain_Max;
	}

	u32 msg = pxiPmMakeMsg(PxiPmMsg_MicSetAmp, gain | (enable << 8));
	pxiSendAndReceive(PxiChannel_Power, msg);
}

bool scfgSetMcPower(bool on)
{
	u32 msg = pxiPmMakeMsg(PxiPmMsg_SetMcPower, on ? 1 : 0);
	return pxiSendAndReceive(PxiChannel_Power, msg);
}
