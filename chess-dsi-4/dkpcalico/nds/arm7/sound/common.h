// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <calico/types.h>
#include <calico/system/mailbox.h>
#include <calico/nds/bios.h>
#include <calico/nds/sound.h>
#include <calico/nds/arm7/sound.h>
#include "../../transfer.h"
#include "../../pxi/sound.h"

//#define SOUND_DEBUG

#ifdef SOUND_DEBUG
#include <calico/system/dietprint.h>
#else
#define dietPrint(...) ((void)0)
#endif

typedef struct SoundState {
	u16 soundcnt_cfg;
	u16 channel_mask;
	u16 pxi_credits;
	bool mixer_sleep_lock;
} SoundState;

extern SoundState g_soundState;

MK_INLINE bool _soundIsEnabled(void)
{
	return g_soundState.soundcnt_cfg & SOUNDCNT_ENABLE;
}

void _soundEnable(void);
void _soundDisable(void);
void _soundSetAutoUpdate(bool enable);
void _soundUpdateSharedState(void);
void _soundPxiProcess(Mailbox* mb, bool do_credit_update);
