// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/arm/common.h>
#include <calico/system/thread.h>
#include <calico/nds/pxi.h>
#include <calico/nds/smutex.h>

static ThrListNode s_smutexWaitList;

void smutexLock(SMutex* m)
{
	uptr self = (uptr)threadGetSelf();
	ArmIrqState st = armIrqLockByPsr();

	bool try_again;
	do {
		while (m->cpu_id != SMUTEX_MY_CPU_ID) {
			if (armSwapWord(1, &m->spinner) == 0) {
				m->cpu_id = SMUTEX_MY_CPU_ID;
				break;
			}
			pxiWaitForPing();
		}

		try_again = m->thread_ptr != 0;
		if (try_again) {
			threadBlock(&s_smutexWaitList, (u32)m);
		} else {
			m->thread_ptr = self;
		}
	} while (try_again);

	armIrqUnlockByPsr(st);
}

bool smutexTryLock(SMutex* m)
{
	uptr self = (uptr)threadGetSelf();
	ArmIrqState st = armIrqLockByPsr();

	bool rc = m->cpu_id == SMUTEX_MY_CPU_ID;
	if_likely (!rc) {
		rc = armSwapWord(1, &m->spinner) == 0;
		if_likely (rc) {
			m->cpu_id = SMUTEX_MY_CPU_ID;
		}
	}
	if_likely (rc) {
		rc = m->thread_ptr == 0;
		if_likely (rc) {
			m->thread_ptr = self;
		}
	}

	armIrqUnlockByPsr(st);
	return rc;
}

void smutexUnlock(SMutex* m)
{
	uptr self = (uptr)threadGetSelf();
	ArmIrqState st = armIrqLockByPsr();

	// Check for naughty callers
	if (m->cpu_id != SMUTEX_MY_CPU_ID || m->thread_ptr != self) {
		armIrqUnlockByPsr(st);
		return;
	}

	m->thread_ptr = 0;
	m->cpu_id = 0;
	armCompilerBarrier(); // Make sure the spinner is cleared *after* the control word
	m->spinner = 0;
	pxiPing();
	threadUnblockAllByValue(&s_smutexWaitList, (u32)m);
	armIrqUnlockByPsr(st);
}
