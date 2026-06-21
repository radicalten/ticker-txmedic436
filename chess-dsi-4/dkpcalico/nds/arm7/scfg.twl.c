// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/thread.h>
#include <calico/nds/scfg.h>

MK_INLINE unsigned _scfgMcGetPowerState(void)
{
	return REG_SCFG_MC & SCFG_MC_POWER_MASK;
}

MK_INLINE void _scfgMcSetPowerState(unsigned state)
{
	REG_SCFG_MC = (REG_SCFG_MC &~ SCFG_MC_POWER_MASK) | (state & SCFG_MC_POWER_MASK);
}

static void _scfgMcPowerOn(void)
{
	// Wait in case there's a scheduled power off request
	while (_scfgMcGetPowerState() == SCFG_MC_POWER_OFF_REQ) {
		threadSleep(1000); // 1ms
	}

	// We only need to do anything if the card is powered off
	if (_scfgMcGetPowerState() == SCFG_MC_POWER_OFF) {
		// Power on the card and assert reset
		threadSleep(1000); // 1ms
		_scfgMcSetPowerState(SCFG_MC_POWER_ON_REQ);

		// Transition to normal power on state
		threadSleep(10000); // 10ms
		_scfgMcSetPowerState(SCFG_MC_POWER_ON);

		// Enforce a delay before the ntrcard owner can deassert reset
		threadSleep(27000); // 27ms

		// XX: We would now set ROMCNT.bit29 and wait 120ms for the card to
		// stabilize. However, we do not necessarily own the NTRCARD registers.
		// For this reason, leave that task up to the user.
	}
}

static void _scfgMcPowerOff(void)
{
	// Wait in case there's already a scheduled power off request
	while (_scfgMcGetPowerState() == SCFG_MC_POWER_OFF_REQ) {
		threadSleep(1000); // 1ms
	}

	// We only need to do anything if the card is powered on
	if (_scfgMcGetPowerState() == SCFG_MC_POWER_ON) {
		// Request power off
		_scfgMcSetPowerState(SCFG_MC_POWER_OFF_REQ);

		// Wait for the card to power off
		while (_scfgMcGetPowerState() != SCFG_MC_POWER_OFF) {
			threadSleep(1000); // 1ms
		}
	}
}

bool scfgSetMcPower(bool on)
{
	if (!scfgIsPresent()) {
		return false;
	}

	if (on) {
		_scfgMcPowerOn();
	} else {
		_scfgMcPowerOff();
	}

	return true;
}
