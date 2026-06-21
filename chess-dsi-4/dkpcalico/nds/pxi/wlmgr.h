// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <calico/nds/pxi.h>
#include <calico/nds/wlmgr.h>

#define PXI_WLMGR_NUM_CREDITS 32

typedef enum PxiWlMgrCmd {
	PxiWlMgrCmd_Start        = 0,
	PxiWlMgrCmd_Stop         = 1,
	PxiWlMgrCmd_StartScan    = 2,
	PxiWlMgrCmd_Associate    = 3,
	PxiWlMgrCmd_Disassociate = 4,
} PxiWlMgrCmd;

typedef struct PxiWlMgrArgAssociate {
	WlanBssDesc const* bss;
	WlanAuthData const* auth;
} PxiWlMgrArgAssociate;

MK_CONSTEXPR u32 pxiWlMgrMakeCmd(PxiWlMgrCmd type, unsigned imm)
{
	return (type & 0x1f) | (imm << 5);
}

MK_CONSTEXPR PxiWlMgrCmd pxiWlMgrCmdGetType(u32 msg)
{
	return (PxiWlMgrCmd)(msg & 0x1f);
}

MK_CONSTEXPR unsigned pxiWlMgrCmdGetImm(u32 msg)
{
	return msg >> 5;
}

MK_CONSTEXPR u32 pxiWlMgrMakeEvent(WlMgrEvent type, unsigned imm)
{
	return (type & 0xf) | (imm << 4);
}

MK_CONSTEXPR WlMgrEvent pxiWlMgrEventGetType(u32 msg)
{
	return (WlMgrEvent)(msg & 0xf);
}

MK_CONSTEXPR unsigned pxiWlMgrEventGetImm(u32 msg)
{
	return msg >> 4;
}
