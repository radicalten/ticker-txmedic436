// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/tick.h>
#include <calico/nds/bios.h>
#include <calico/nds/keypad.h>
#include "transfer.h"

#if defined(ARM9)

unsigned keypadGetState(void)
{
	return keypadGetInState() | s_transferRegion->keypad_ext;
}

#elif defined(ARM7)

static TickTask s_keypadTask;

static void _keypadSendExtToArm9(TickTask* t)
{
	s_transferRegion->keypad_ext = keypadGetExtState();
}

void keypadStartExtServer(void)
{
	// XX: Wait for RCNT_EXT to stabilize immediately after bootup.
	// This behavior has only been observed on emulator so far.
	while (REG_RCNT_EXT == 0) {
		svcWaitByLoop(0x1000);
	}

	tickTaskStart(&s_keypadTask, _keypadSendExtToArm9, 0, ticksFromUsec(4000));
}

#endif
