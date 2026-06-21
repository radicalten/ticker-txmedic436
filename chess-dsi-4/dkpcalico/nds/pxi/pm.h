// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <calico/nds/pxi.h>
#include <calico/nds/pm.h>

typedef enum PxiPmMsgType {

	PxiPmMsg_GetBatteryState = 0,
	PxiPmMsg_Sleep           = 1,
	PxiPmMsg_Wakeup          = 2,
	PxiPmMsg_ReadNvram       = 3,
	PxiPmMsg_MicSetAmp       = 4,
	PxiPmMsg_SetPowerLed     = 5,
	PxiPmMsg_SetMcPower      = 6,

} PxiPmMsgType;

MK_CONSTEXPR u32 pxiPmMakeMsg(PxiPmMsgType type, unsigned imm)
{
	return (type & 0xff) | (imm << 8);
}

MK_CONSTEXPR PxiPmMsgType pxiPmGetType(u32 msg)
{
	return (PxiPmMsgType)(msg & 0xff);
}

MK_CONSTEXPR unsigned pxiPmGetImmediate(u32 msg)
{
	return msg >> 8;
}
