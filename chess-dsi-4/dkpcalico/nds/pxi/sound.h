// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <calico/nds/pxi.h>

#define PXI_SOUND_NUM_CREDITS 32
#define PXI_SOUND_CREDIT_UPDATE_THRESHOLD 4

typedef enum PxiSoundCmd {
	PxiSoundCmd_Synchronize    = 0,
	PxiSoundCmd_SetPower       = 1,
	PxiSoundCmd_SetAutoUpdate  = 2,
	PxiSoundCmd_SetChannelMask = 3,
	PxiSoundCmd_Start          = 4,
	PxiSoundCmd_Stop           = 5,
	PxiSoundCmd_PreparePcm     = 6,
	PxiSoundCmd_PreparePsg     = 7,
	PxiSoundCmd_SetVolume      = 8,
	PxiSoundCmd_SetPan         = 9,
	PxiSoundCmd_SetTimer       = 10,
	PxiSoundCmd_SetDuty        = 11,
	PxiSoundCmd_SetMixerVolume = 12,
	PxiSoundCmd_SetMixerConfig = 13,
	PxiSoundCmd_PrepareCap     = 14,
	PxiSoundCmd_SetMixerSleep  = 15,
} PxiSoundCmd;

typedef enum PxiSoundEvent {
	PxiSoundEvent_UpdateCredits = 0,
} PxiSoundEvent;

typedef struct PxiSoundArgPreparePcm {
	u32 sad    : 22;
	u32 mode   : 2;  // SoundMode
	u32 vol    : 7;
	u32 start  : 1;

	u32 timer  : 16;
	u32 pnt    : 16;

	u32 len    : 22;
	u32 voldiv : 2;  // SoundVolDiv
	u32 fmt    : 2;  // SoundFmt
	u32 _pad   : 2;
	u32 ch     : 4;
} PxiSoundArgPreparePcm;

typedef struct PxiSoundArgPreparePsg {
	u32 timer  : 16;
	u32 vol    : 7;
	u32 voldiv : 2;  // SoundVolDiv
	u32 duty   : 3;  // SoundDuty
	u32 ch     : 3;
	u32 start  : 1;
} PxiSoundArgPreparePsg;

typedef union PxiSoundImmStartStop {
	unsigned imm;
	struct {
		unsigned ch_mask  : 16;
		unsigned cap_mask : 2;
	};
} PxiSoundImmStartStop;

typedef union PxiSoundImmSetVolume {
	unsigned imm;
	struct {
		unsigned ch     : 4;
		unsigned vol    : 7;
		unsigned voldiv : 2; // SoundVolDiv
	};
} PxiSoundImmSetVolume;

typedef union PxiSoundImmSetPan {
	unsigned imm;
	struct {
		unsigned ch  : 4;
		unsigned pan : 7;
	};
} PxiSoundImmSetPan;

typedef union PxiSoundImmSetTimer {
	unsigned imm;
	struct {
		unsigned ch    : 4;
		unsigned timer : 16;
	};
} PxiSoundImmSetTimer;

typedef union PxiSoundImmSetDuty {
	unsigned imm;
	struct {
		unsigned ch   : 3;
		unsigned duty : 3; // SoundDuty
	};
} PxiSoundImmSetDuty;

typedef union PxiSoundImmPrepareCap {
	unsigned imm;
	struct {
		unsigned cap    : 1;
		unsigned config : 4;
	};
} PxiSoundImmPrepareCap;

typedef struct PxiSoundArgPrepareCap {
	u32 dad;
	u32 len;
} PxiSoundArgPrepareCap;

MK_CONSTEXPR u32 pxiSoundMakeCmdMsg(PxiSoundCmd cmd, bool update_credits, unsigned imm)
{
	return (cmd & 0x7f) | (update_credits ? (1U<<7) : 0) | (imm << 8);
}

MK_CONSTEXPR PxiSoundCmd pxiSoundCmdGetType(u32 msg)
{
	return (PxiSoundCmd)(msg & 0x7f);
}

MK_CONSTEXPR bool pxiSoundCmdIsUpdateCredits(u32 msg)
{
	return (msg >> 7) & 1;
}

MK_CONSTEXPR unsigned pxiSoundCmdGetImm(u32 msg)
{
	return msg >> 8;
}

MK_CONSTEXPR u32 pxiSoundMakeEventMsg(PxiSoundEvent evt, unsigned imm)
{
	return (evt & 0xf) | (imm << 4);
}

MK_CONSTEXPR PxiSoundEvent pxiSoundEventGetType(u32 msg)
{
	return (PxiSoundEvent)(msg & 0xf);
}

MK_CONSTEXPR unsigned pxiSoundEventGetImm(u32 msg)
{
	return msg >> 4;
}
