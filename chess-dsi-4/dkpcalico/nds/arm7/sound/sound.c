// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include "common.h"
#include <calico/system/thread.h>
#include <calico/nds/pm.h>

#define SOUND_NUM_MAIL_SLOTS 4
#define SOUND_MAIL_EXIT   0
#define SOUND_MAIL_WAKEUP 1
#define SOUND_MAIL_TIMER  2

// XX: Official code has a rounding error in the value for the timer period,
//     which we're incorporating too for compatibility.
#define SOUND_TIMER_PERIOD (ticksFromHz(SOUND_UPDATE_HZ) + 1)

SoundState g_soundState;

static Thread s_soundSrvThread;
alignas(8) static u8 s_soundSrvThreadStack[1024];
static TickTask s_soundTimer;
static PmEventCookie s_soundPmCookie;

static Mailbox s_soundSrvMailbox, s_soundPxiMailbox;
static u32 s_soundPxiMailboxSlots[PXI_SOUND_NUM_CREDITS];

static void _soundTimerTick(TickTask* task)
{
	// Send timer message to sound server thread
	mailboxTrySend(&s_soundSrvMailbox, SOUND_MAIL_TIMER);
}

static void _soundPxiHandler(void* user, u32 data)
{
	// Add message to PXI mailbox
	mailboxTrySend((Mailbox*)user, data);

	// Wake up sound server thread if needed
	if (s_soundSrvMailbox.pending_slots == 0) {
		mailboxTrySend(&s_soundSrvMailbox, SOUND_MAIL_WAKEUP);
	}
}

static void _soundPmEventHandler(void* user, PmEvent event)
{
	switch (event) {
		default: break;

		case PmEvent_OnSleep: {
			_soundDisable();
			break;
		}

		case PmEvent_OnWakeup: {
			_soundEnable();
			break;
		}

		case PmEvent_OnReset: {
			while (!mailboxTrySend(&s_soundSrvMailbox, SOUND_MAIL_EXIT)) {
				threadSleep(1000);
			}
			threadJoin(&s_soundSrvThread);
			break;
		}
	}
}

static void _soundReset(void)
{
	// Clear sound registers
	REG_SNDCAPCNT = 0;
	for (unsigned i = 0; i < SOUND_NUM_CHANNELS; i ++) {
		REG_SOUNDxCNT(i) = 0;
	}
}

static int _soundSrvThreadMain(void* arg)
{
	// TODO: Init DSi I2S interface

	// Set up PXI and mailboxes
	u32 slots[SOUND_NUM_MAIL_SLOTS];
	mailboxPrepare(&s_soundSrvMailbox, slots, SOUND_NUM_MAIL_SLOTS);
	mailboxPrepare(&s_soundPxiMailbox, s_soundPxiMailboxSlots, PXI_SOUND_NUM_CREDITS);
	pxiSetHandler(PxiChannel_Sound, _soundPxiHandler, &s_soundPxiMailbox);

	// Enable and initialize sound hardware
	g_soundState.soundcnt_cfg = SOUNDCNT_VOL(0x7f);
	_soundEnable();
	_soundReset();

	// Register PM event handler
	pmAddEventHandler(&s_soundPmCookie, _soundPmEventHandler, NULL);

	for (;;) {
		// Get message
		u32 msg = mailboxRecv(&s_soundSrvMailbox);
		if (msg == SOUND_MAIL_EXIT) {
			break;
		}
		bool is_timer = msg == SOUND_MAIL_TIMER;

		// TODO: Commit channel state to registers

		// Process PXI commands
		_soundPxiProcess(&s_soundPxiMailbox, is_timer);

		// TODO: Process sequencer + sequencer voices

		// Update shared state
		_soundUpdateSharedState();
	}

	// Clean up the sound hardware
	_soundReset();

	return 0;
}

void soundStartServer(u8 thread_prio)
{
	threadPrepare(&s_soundSrvThread, _soundSrvThreadMain, NULL, &s_soundSrvThreadStack[sizeof(s_soundSrvThreadStack)], thread_prio);
	threadStart(&s_soundSrvThread);
}

void _soundEnable(void)
{
	// Do nothing if sound is already enabled
	if (_soundIsEnabled()) {
		return;
	}

	// Enable sound mixer circuit
	pmPowerOn(POWCNT_SOUND);

	// Enable speaker amp
	pmSoundSetAmpPower(true);

	// Smoothly bring up the output signal baseline
	svcSoundBias(true, 0x80);

	// Configure sound mixer
	REG_SOUNDCNT = (g_soundState.soundcnt_cfg |= SOUNDCNT_ENABLE);
}

void _soundDisable(void)
{
	// Do nothing if sound is already disabled
	if (!_soundIsEnabled()) {
		return;
	}

	// Disable sound mixer, backing up configuration
	g_soundState.soundcnt_cfg = (REG_SOUNDCNT &~ SOUNDCNT_ENABLE);
	REG_SOUNDCNT = g_soundState.mixer_sleep_lock ? SOUNDCNT_ENABLE : 0;

	// Smoothly drop the output signal baseline
	svcSoundBias(false, 0x80);

	// Disable speaker amp
	pmSoundSetAmpPower(false);

	// Disable sound mixer circuit
	if (!g_soundState.mixer_sleep_lock) {
		pmPowerOff(POWCNT_SOUND);
	}
}

void _soundSetAutoUpdate(bool enable)
{
	// Do nothing if tick task is already started/stopped
	bool is_enabled = s_soundTimer.fn != NULL;
	if (enable == is_enabled) {
		return;
	}

	// Start or stop tick task for periodic sound updates
	if (enable) {
		tickTaskStart(&s_soundTimer, _soundTimerTick, 0, SOUND_TIMER_PERIOD);
	} else {
		tickTaskStop(&s_soundTimer);
	}
}

void _soundUpdateSharedState(void)
{
	unsigned ch_mask = 0;
	for (unsigned i = 0; i < SOUND_NUM_CHANNELS; i ++) {
		if (soundChIsActive(i)) {
			ch_mask |= 1U<<i;
		}
	}

	s_transferRegion->sound_active_ch_mask = ch_mask;
}
