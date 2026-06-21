// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/nds/arm9/sound.h>
#include "../transfer.h"
#include "../pxi/sound.h"

static bool s_soundInit, s_soundAutoUpdate;
static ThrListNode s_soundPxiCreditWaitList;
static u16 s_soundPxiCredits;

static void _soundPxiHandler(void* user, u32 data)
{
	PxiSoundEvent evt = pxiSoundEventGetType(data);
	unsigned imm = pxiSoundEventGetImm(data);

	switch (evt) {
		default: break;

		case PxiSoundEvent_UpdateCredits: {
			s_soundPxiCredits += imm;
			if (s_soundPxiCreditWaitList.next) {
				threadUnblockOneByValue(&s_soundPxiCreditWaitList, (u32)user);
			}
			break;
		}
	}
}

MK_NOINLINE static bool _soundPxiCheckCredits(unsigned needed_credits)
{
	// If we don't have enough credits, wait until we do
	ArmIrqState st = armIrqLockByPsr();
	unsigned avail;
	while ((avail = s_soundPxiCredits) < needed_credits) {
		threadBlock(&s_soundPxiCreditWaitList, 0); // izQKJJ9tyZc
	}
	avail -= needed_credits;
	s_soundPxiCredits = avail;
	armIrqUnlockByPsr(st);

	return avail < PXI_SOUND_CREDIT_UPDATE_THRESHOLD;
}

MK_INLINE void _soundIssueCmdAsync(PxiSoundCmd cmd, unsigned imm, const void* arg, size_t arg_size)
{
	unsigned arg_size_words = (arg_size + 3) / 4;
	bool update_credits = _soundPxiCheckCredits(1 + arg_size_words);
	unsigned msg = pxiSoundMakeCmdMsg(cmd, update_credits, imm);
	if (arg_size) {
		pxiSendWithData(PxiChannel_Sound, msg, (const u32*)arg, arg_size_words);
	} else {
		pxiSend(PxiChannel_Sound, msg);
	}
}

void soundInit(void)
{
	if (s_soundInit) {
		return;
	}

	s_soundInit = true;
	s_soundPxiCredits = PXI_SOUND_NUM_CREDITS;
	pxiSetHandler(PxiChannel_Sound, _soundPxiHandler, NULL);
	pxiWaitRemote(PxiChannel_Sound);
	soundPowerOn();
}

void soundSynchronize(void)
{
	bool update_credits = _soundPxiCheckCredits(1);
	pxiSendAndReceive(PxiChannel_Sound, pxiSoundMakeCmdMsg(PxiSoundCmd_Synchronize, update_credits, 0));
}

void soundSetPower(bool enable)
{
	_soundIssueCmdAsync(PxiSoundCmd_SetPower, enable ? 1 : 0, NULL, 0);
}

void soundSetAutoUpdate(bool enable)
{
	if (s_soundAutoUpdate == enable) {
		return;
	}

	s_soundAutoUpdate = enable;
	_soundIssueCmdAsync(PxiSoundCmd_SetAutoUpdate, enable ? 1 : 0, NULL, 0);
}

unsigned soundGetActiveChannels(void)
{
	if (!s_soundAutoUpdate) {
		soundSynchronize();
	}

	return s_transferRegion->sound_active_ch_mask;
}

void soundSetMixerVolume(unsigned vol)
{
	_soundIssueCmdAsync(PxiSoundCmd_SetMixerVolume, vol, NULL, 0);
}

void soundSetMixerConfigDirect(unsigned config)
{
	_soundIssueCmdAsync(PxiSoundCmd_SetMixerConfig, config, NULL, 0);
}

void soundSetMixerSleep(bool enable)
{
	_soundIssueCmdAsync(PxiSoundCmd_SetMixerSleep, enable ? 1 : 0, NULL, 0);
}

MK_INLINE SoundVolDiv _soundCalcVolDiv(unsigned* vol)
{
	if (*vol < 0x80) {
		return SoundVolDiv_16;
	} else if (*vol < 0x200) {
		*vol /= 4;
		return SoundVolDiv_4;
	} else if (*vol < 0x400) {
		*vol /= 8;
		return SoundVolDiv_2;
	} else if (*vol < 0x800) {
		*vol /= 16;
		return SoundVolDiv_1;
	} else {
		*vol = 0x7f;
		return SoundVolDiv_1;
	}
}

void soundPreparePcm(
	unsigned ch, unsigned vol, unsigned pan, unsigned timer,
	SoundMode mode, SoundFmt fmt, const void* sad, unsigned pnt, unsigned len)
{
	SoundVolDiv voldiv = _soundCalcVolDiv(&vol);
	PxiSoundArgPreparePcm arg = {
		.sad    = ((u32)sad - MM_MAINRAM) / 4,
		.mode   = mode,
		.vol    = vol,
		.start  = (ch & SOUND_START) != 0,

		.timer  = timer,
		.pnt    = pnt,

		.len    = len,
		.voldiv = voldiv,
		.fmt    = fmt,
		._pad   = 0,
		.ch     = ch,
	};

	_soundIssueCmdAsync(PxiSoundCmd_PreparePcm, pan, &arg, sizeof(arg));
}

void soundPreparePsg(unsigned ch, unsigned vol, unsigned pan, unsigned timer, SoundDuty duty)
{
	SoundVolDiv voldiv = _soundCalcVolDiv(&vol);
	PxiSoundArgPreparePsg arg = {
		.timer  = timer,
		.vol    = vol,
		.voldiv = voldiv,
		.duty   = duty,
		.ch     = ch-8,
		.start  = (ch & SOUND_START) != 0,
	};

	_soundIssueCmdAsync(PxiSoundCmd_PreparePsg, pan, &arg, sizeof(arg));
}

void soundPrepareCapDirect(unsigned cap, unsigned config, void* dad, unsigned len)
{
	PxiSoundImmPrepareCap u = {
		.cap    = cap,
		.config = config,
	};

	PxiSoundArgPrepareCap arg = {
		.dad = (u32)dad,
		.len = len,
	};

	_soundIssueCmdAsync(PxiSoundCmd_PrepareCap, u.imm, &arg, sizeof(arg));
}

void soundStart(u32 mask)
{
	_soundIssueCmdAsync(PxiSoundCmd_Start, mask, NULL, 0);
}

void soundStop(u32 mask)
{
	_soundIssueCmdAsync(PxiSoundCmd_Stop, mask, NULL, 0);
}

void soundChSetVolume(unsigned ch, unsigned vol)
{
	SoundVolDiv voldiv = _soundCalcVolDiv(&vol);
	PxiSoundImmSetVolume u = {
		.ch     = ch,
		.vol    = vol,
		.voldiv = voldiv,
	};

	_soundIssueCmdAsync(PxiSoundCmd_SetVolume, u.imm, NULL, 0);
}

void soundChSetPan(unsigned ch, unsigned pan)
{
	PxiSoundImmSetPan u = {
		.ch  = ch,
		.pan = pan,
	};

	_soundIssueCmdAsync(PxiSoundCmd_SetPan, u.imm, NULL, 0);
}

void soundChSetTimer(unsigned ch, unsigned timer)
{
	PxiSoundImmSetTimer u = {
		.ch    = ch,
		.timer = timer,
	};

	_soundIssueCmdAsync(PxiSoundCmd_SetTimer, u.imm, NULL, 0);
}

void soundChSetDuty(unsigned ch, SoundDuty duty)
{
	PxiSoundImmSetDuty u = {
		.ch   = ch-8,
		.duty = duty,
	};

	_soundIssueCmdAsync(PxiSoundCmd_SetDuty, u.imm, NULL, 0);
}
