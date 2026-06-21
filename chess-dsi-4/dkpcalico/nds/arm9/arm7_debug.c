// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/nds/pxi.h>
#include <calico/nds/arm9/arm7_debug.h>
#include "../transfer.h"

static Thread s_arm7DebugThread;
alignas(8) static u8 s_arm7DebugStack[8*1024];

MK_WEAK void* g_arm7DebugStackTop = &s_arm7DebugStack[sizeof(s_arm7DebugStack)];

static int _arm7DebugThread(void* data)
{
	Arm7DebugFn fn = (Arm7DebugFn)data;

	s_debugBuf->flags = DBG_BUF_ALIVE;
	pxiPing();

	for (;;) {
		u16 flags = s_debugBuf->flags;
		if (!(flags & DBG_BUF_BUSY)) {
			pxiWaitForPing();
			continue;
		}

		if_likely (s_debugBuf->size) {
			fn(s_debugBuf->data, s_debugBuf->size);
			s_debugBuf->size = 0;
		}

		s_debugBuf->flags = flags &~ DBG_BUF_BUSY;
		pxiPing();
	}

	return 0;
}

void installArm7DebugSupport(Arm7DebugFn fn, u8 thread_prio)
{
	threadPrepare(&s_arm7DebugThread, _arm7DebugThread, fn, g_arm7DebugStackTop, thread_prio);
	threadAttachLocalStorage(&s_arm7DebugThread, NULL);
	threadStart(&s_arm7DebugThread);
}
