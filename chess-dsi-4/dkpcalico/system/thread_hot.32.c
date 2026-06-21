// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include "thread-priv.h"

ThrSchedState __sched_state;
IrqHandler __irq_table[MK_IRQ_NUM_HANDLERS];

void threadSwitchTo(Thread* t, ArmIrqState st)
{
	if_likely ((armGetCpsr() & ARM_PSR_MODE_MASK) == ARM_PSR_MODE_IRQ) {
		if (!s_deferredThread || t->prio < s_deferredThread->prio)
			s_deferredThread = t;
		armIrqUnlockByPsr(st);
		return;
	}

	if (!armContextSave(&s_curThread->ctx, st, 1)) {
		s_curThread = t;
		armContextLoad(&t->ctx);
	}
}

u32 threadBlock(ThrListNode* queue, u32 token)
{
	Thread* self = s_curThread;
	ArmIrqState st = armIrqLockByPsr();

	self->status = ThrStatus_Waiting;
	self->token = token;
	threadLinkEnqueue(queue, self);

	Thread* next = threadFindRunnable(s_firstThread);
	threadSwitchTo(next, st);

	return self->token;
}

MK_INLINE void _threadUnblockCommon(ThrListNode* queue, int max, ThrUnblockMode mode, u32 ref)
{
	ArmIrqState st = armIrqLockByPsr();
	Thread* resched = NULL;
	Thread* next;

	for (Thread* cur = queue->next; max != 0 && cur; cur = next) {
		next = cur->link.next;
		if (!threadTestUnblock(cur, mode, ref)) {
			continue;
		}

		threadLinkDequeue(queue, cur);

		if (mode == ThrUnblockMode_ByMask) {
			cur->token &= ref;
		} else {
			cur->token = 1;
		}

		if (!cur->pause) {
			cur->status = ThrStatus_Running;
			if (!resched) {
				resched = cur; // Remember the first unblocked (highest priority) thread
			}
		}

		if (max > 0) {
			--max;
		}
	}

	threadReschedule(resched, st);
}

void threadUnblockOneByValue(ThrListNode* queue, u32 ref)
{
	_threadUnblockCommon(queue, +1, ThrUnblockMode_ByValue, ref);
}

void threadUnblockOneByMask(ThrListNode* queue, u32 ref)
{
	_threadUnblockCommon(queue, +1, ThrUnblockMode_ByMask, ref);
}

void threadUnblockAllByValue(ThrListNode* queue, u32 ref)
{
	_threadUnblockCommon(queue, -1, ThrUnblockMode_ByValue, ref);
}

void threadUnblockAllByMask(ThrListNode* queue, u32 ref)
{
	_threadUnblockCommon(queue, -1, ThrUnblockMode_ByMask, ref);
}

void threadBlockCancel(ThrListNode* queue, Thread* t)
{
	ArmIrqState st = armIrqLockByPsr();
	Thread* resched = NULL;

	if (t->status != ThrStatus_Waiting || t->queue != queue) {
		armIrqUnlockByPsr(st);
		return;
	}

	threadLinkDequeue(queue, t);

	t->token = 0;

	if (!t->pause) {
		t->status = ThrStatus_Running;
		resched = t;
	}

	threadReschedule(resched, st);
}
