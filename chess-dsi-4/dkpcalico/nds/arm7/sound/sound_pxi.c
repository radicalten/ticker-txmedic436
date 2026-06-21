// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include "common.h"

MK_INLINE bool _soundBitmaskUnpack(unsigned* mask, unsigned* i)
{
	if (!*mask) {
		return false;
	}

	while (!(*mask & (1U << *i))) {
		++*i;
	}

	*mask &= ~(1U << *i);
	return true;
}

MK_NOINLINE MK_CODE32 static void _soundPxiStart(unsigned ch_mask, u16 cap_mask)
{
	IrqState st = irqLock();

	for (unsigned i = 0; _soundBitmaskUnpack(&ch_mask, &i);) {
		soundChStart(i);
	}

	if (cap_mask) {
		soundCapStart(cap_mask);
	}

	irqUnlock(st);
}

MK_NOINLINE MK_CODE32 static void _soundPxiStop(unsigned ch_mask, u16 cap_mask)
{
	IrqState st = irqLock();

	for (unsigned i = 0; _soundBitmaskUnpack(&ch_mask, &i);) {
		soundChStop(i);
	}

	if (cap_mask) {
		soundCapStop(cap_mask);
	}

	irqUnlock(st);
}

MK_NOINLINE MK_CODE32 static void _soundPxiProcessCmd(PxiSoundCmd cmd, unsigned imm, const void* body, unsigned num_words)
{
	// Ignore commands when sound hardware is not enabled
	if (cmd > PxiSoundCmd_SetPower && !_soundIsEnabled()) {
		dietPrint("[SND] off; ignoring cmd%u\n", cmd);
		return;
	}

	switch (cmd) {
		default: {
			dietPrint("[SND] unkcmd%u 0x%X + %u\n", cmd, imm, num_words);
			break;
		}

		case PxiSoundCmd_Synchronize: {
			_soundUpdateSharedState();
			pxiReply(PxiChannel_Sound, 0);
			break;
		}

		case PxiSoundCmd_SetPower: {
			if (imm & 1) {
				_soundEnable();
			} else {
				_soundDisable();
			}
			break;
		}

		case PxiSoundCmd_SetAutoUpdate: {
			_soundSetAutoUpdate(imm & 1);
			break;
		}

		case PxiSoundCmd_Start: {
			PxiSoundImmStartStop u = { imm };
			_soundPxiStart(u.ch_mask, soundExpandCapMask(u.cap_mask));
			_soundUpdateSharedState();
			break;
		}

		case PxiSoundCmd_Stop: {
			PxiSoundImmStartStop u = { imm };
			_soundPxiStop(u.ch_mask, soundExpandCapMask(u.cap_mask));
			_soundUpdateSharedState();
			break;
		}

		case PxiSoundCmd_PreparePcm: {
			const PxiSoundArgPreparePcm* arg = (const PxiSoundArgPreparePcm*)body;
			soundChPreparePcm(
				arg->ch, arg->vol, (SoundVolDiv)arg->voldiv, imm, arg->timer,
				(SoundMode)arg->mode, (SoundFmt)arg->fmt, (const void*)(MM_MAINRAM + arg->sad*4), arg->pnt, arg->len);

			if (arg->start) {
				soundChStart(arg->ch);
				s_transferRegion->sound_active_ch_mask |= 1U << arg->ch;
			}
			break;
		}

		case PxiSoundCmd_PreparePsg: {
			const PxiSoundArgPreparePsg* arg = (const PxiSoundArgPreparePsg*)body;
			unsigned ch = arg->ch + 8;
			soundChPreparePsg(ch, arg->vol, (SoundVolDiv)arg->voldiv, imm, arg->timer, (SoundDuty)arg->duty);

			if (arg->start) {
				soundChStart(ch);
				s_transferRegion->sound_active_ch_mask |= 1U << ch;
			}
			break;
		}

		case PxiSoundCmd_SetVolume: {
			PxiSoundImmSetVolume u = { imm };
			soundChSetVolume(u.ch, u.vol, (SoundVolDiv)u.voldiv);
			break;
		}

		case PxiSoundCmd_SetPan: {
			PxiSoundImmSetPan u = { imm };
			soundChSetPan(u.ch, u.pan);
			break;
		}

		case PxiSoundCmd_SetTimer: {
			PxiSoundImmSetTimer u = { imm };
			soundChSetTimer(u.ch, u.timer);
			break;
		}

		case PxiSoundCmd_SetDuty: {
			PxiSoundImmSetDuty u = { imm };
			soundChSetDuty(u.ch+8, (SoundDuty)u.duty);
			break;
		}

		case PxiSoundCmd_SetMixerVolume: {
			soundSetMixerVolume(imm);
			break;
		}

		case PxiSoundCmd_SetMixerConfig: {
			soundSetMixerConfigDirect(imm);
			break;
		}

		case PxiSoundCmd_PrepareCap: {
			PxiSoundImmPrepareCap u = { imm };
			const PxiSoundArgPrepareCap* arg = (const PxiSoundArgPrepareCap*)body;
			soundPrepareCapDirect(u.cap, u.config, (void*)arg->dad, arg->len);
			break;
		}

		case PxiSoundCmd_SetMixerSleep: {
			g_soundState.mixer_sleep_lock = !(imm & 1);
			break;
		}
	}
}

void _soundPxiProcess(Mailbox* mb, bool do_credit_update)
{
	u32 cmdheader;
	while (mailboxTryRecv(mb, &cmdheader)) {
		unsigned num_words = cmdheader >> 26;
		if (num_words) {
			cmdheader &= (1U<<26)-1;
		}

		PxiSoundCmd cmd = pxiSoundCmdGetType(cmdheader);
		unsigned imm = pxiSoundCmdGetImm(cmdheader);
		do_credit_update |= pxiSoundCmdIsUpdateCredits(cmdheader);
		g_soundState.pxi_credits += 1 + num_words;

		if_likely (num_words == 0) {
			// Process command directly
			_soundPxiProcessCmd(cmd, imm, NULL, 0);
		} else {
			// Receive message body
			u32 body[num_words];
			for (unsigned i = 0; i < num_words; i ++) {
				body[i] = mailboxRecv(mb);
			}

			// Process command
			_soundPxiProcessCmd(cmd, imm, body, num_words);
		}
	}

	if (do_credit_update && g_soundState.pxi_credits) {
		unsigned credits = g_soundState.pxi_credits;
		g_soundState.pxi_credits = 0;
		dietPrint("[SND] Used %u credits\n", credits);
		pxiSend(PxiChannel_Sound, pxiSoundMakeEventMsg(PxiSoundEvent_UpdateCredits, credits));
	}
}
