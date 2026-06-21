// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <string.h>
#include <calico/types.h>
#include <calico/arm/common.h>
#include <calico/system/irq.h>
#include <calico/system/thread.h>

extern ThrSchedState __sched_state;

#define s_curThread __sched_state.cur
#define s_deferredThread __sched_state.deferred
#define s_firstThread __sched_state.first
#define s_irqWaitMask __sched_state.irqWaitMask
#define s_irqWaitList __sched_state.irqWaitList
#if MK_IRQ_NUM_HANDLERS > 32
#define s_irqWaitMask2 __sched_state.irqWaitMask2
#define s_irqWaitList2 __sched_state.irqWaitList2
#endif

typedef enum ThrUnblockMode {
	ThrUnblockMode_Any,
	ThrUnblockMode_ByValue,
	ThrUnblockMode_ByMask,
} ThrUnblockMode;

MK_INLINE Thread* threadGetInsertPosition(Thread* t)
{
	Thread* pos = NULL;
	for (Thread* cur = s_firstThread; cur && cur->prio <= t->prio; cur = cur->next)
		pos = cur;
	return pos;
}

MK_INLINE Thread* threadGetPrevious(Thread* t)
{
	for (Thread* cur = s_firstThread; cur; cur = cur->next) {
		if (cur->next == t) {
			return cur;
		}
	}
	return NULL;
}

MK_INLINE void threadEnqueue(Thread* t)
{
	Thread* pos = threadGetInsertPosition(t);
	if (pos) {
		t->next = pos->next;
		pos->next = t;
	} else {
		t->next = s_firstThread;
		s_firstThread = t;
	}
}

MK_INLINE void threadDequeue(Thread* t)
{
	if (s_firstThread == t) {
		s_firstThread = t->next;
	} else {
		Thread* prev = threadGetPrevious(t);
		if (prev) {
			prev->next = t->next;
		}
	}
}

MK_INLINE Thread* threadFindRunnable(Thread* first)
{
	Thread* t;
	for (t = first; t && t->status != ThrStatus_Running; t = t->next);
	return t;
}

MK_INLINE Thread* threadLinkGetInsertPosition(ThrListNode* queue, Thread* t)
{
	Thread* pos;
	for (pos = queue->next; pos && pos->prio <= t->prio; pos = pos->link.next);
	return pos ? pos->link.prev : queue->prev;
}

MK_INLINE void threadLinkEnqueue(ThrListNode* queue, Thread* t)
{
	Thread* pos = threadLinkGetInsertPosition(queue, t);
	if (pos) {
		t->link.next   = pos->link.next;
		t->link.prev   = pos;
		pos->link.next = t;
	} else {
		t->link.next   = queue->next;
		t->link.prev   = NULL;
		queue->next    = t;
	}
	if (t->link.next)
		t->link.next->link.prev = t;
	else
		queue->prev = t;
	t->queue = queue;
}

MK_INLINE void threadLinkDequeue(ThrListNode* queue, Thread* t)
{
	if (t->queue != queue)
		return;
	t->queue = NULL;
	(t->link.prev ? &t->link.prev->link : queue)->next = t->link.next;
	(t->link.next ? &t->link.next->link : queue)->prev = t->link.prev;
}

MK_INLINE bool threadTestUnblock(Thread* t, ThrUnblockMode mode, u32 ref)
{
	switch (mode) {
		default:
		case ThrUnblockMode_Any:
			return true;
		case ThrUnblockMode_ByValue:
			return t->token == ref;
		case ThrUnblockMode_ByMask:
			return (t->token & ref) != 0;
	}
}

MK_EXTERN32 void threadSwitchTo(Thread* t, ArmIrqState st);

MK_INLINE void threadReschedule(Thread* t, ArmIrqState st)
{
	if (t && t->prio < s_curThread->prio) {
		threadSwitchTo(t, st);
	} else {
		armIrqUnlockByPsr(st);
	}
}

void threadUpdateDynamicPrio(Thread* t);
