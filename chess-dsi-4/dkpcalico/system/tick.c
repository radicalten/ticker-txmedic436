// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/irq.h>
#include <calico/system/tick.h>
#include <calico/gba/timer.h>

static bool s_tickInit;
static vu64 s_highTickCount;
static TickTask* s_firstTask;

MK_CONSTEXPR bool _tickIsSequential32(u32 lhs, u32 rhs)
{
	return (s32)(rhs - lhs) > 0;
}

MK_INLINE TickTask* _tickTaskGetInsertPosition(TickTask* t)
{
	TickTask* pos = NULL;
	for (TickTask* cur = s_firstTask; cur && _tickIsSequential32(cur->target, t->target); cur = cur->next)
		pos = cur;
	return pos;
}

MK_INLINE TickTask* _tickTaskGetPrevious(TickTask* t)
{
	TickTask* pos = NULL;
	for (TickTask* cur = s_firstTask; cur && cur != t; cur = cur->next)
		pos = cur;
	return pos;
}

MK_INLINE void _tickTaskEnqueue(TickTask* t)
{
	TickTask* pos = _tickTaskGetInsertPosition(t);
	if (pos) {
		t->next = pos->next;
		pos->next = t;
	} else {
		t->next = s_firstTask;
		s_firstTask = t;
	}
}

MK_INLINE void _tickTaskDequeue(TickTask* t)
{
	TickTask* prev = _tickTaskGetPrevious(t);
	if (prev)
		prev->next = t->next;
	else
		s_firstTask = t->next;
}

static void _tickTaskSchedule(TickTask* t)
{
	REG_TMxCNT_H(3) = 0;
	if_likely (!t) {
		return;
	}

	s32 diff = t->target - (s32)tickGetCount();
	u16 preload = 0;
	if (diff <= 0) {
		preload = -1;
	} else if (diff < 0x10000) {
		preload = -diff;
	}

	REG_TMxCNT_L(3) = preload;
	REG_TMxCNT_H(3) = TIMER_PRESCALER_64 | TIMER_ENABLE_IRQ | TIMER_ENABLE;
}

static void _tickCountIsr(void)
{
	s_highTickCount ++;
}

static void _tickTaskIsr(void)
{
	while (s_firstTask && !_tickIsSequential32(tickGetCount(), s_firstTask->target)) {
		TickTask* cur = s_firstTask;
		s_firstTask = cur->next;

		cur->fn(cur);

		if_likely (cur->period != 0 && cur->fn) {
			cur->target += cur->period;
			_tickTaskEnqueue(cur);
		} else {
			cur->fn = NULL;
		}
	}

	_tickTaskSchedule(s_firstTask);
}

void tickInit(void)
{
	IrqState st = irqLock();
	if_unlikely (s_tickInit) {
		irqUnlock(st);
		return;
	}

	// Initialize timer2 (used for the monotonic tick counter)
	REG_TMxCNT_H(2) = 0;
	REG_TMxCNT_L(2) = 0;
	REG_TMxCNT_H(2) = TIMER_PRESCALER_64 | TIMER_ENABLE | TIMER_ENABLE_IRQ;

	// Initialize timer3 (used for task scheduling)
	REG_TMxCNT_H(3) = 0;

	// Set up ISRs
	irqSet(IRQ_TIMER2, _tickCountIsr);
	irqSet(IRQ_TIMER3, _tickTaskIsr);
	irqEnable(IRQ_TIMER2 | IRQ_TIMER3);

	s_tickInit = true;
	irqUnlock(st);
}

u64 tickGetCount(void)
{
	IrqState st = irqLock();

	u16 lo = REG_TMxCNT_L(2);
	u64 hi = s_highTickCount;

	if_unlikely ((REG_IF & IRQ_TIMER2) && !(lo & (1U << 15))) {
		hi ++;
	}

	irqUnlock(st);

	return lo | (hi << 16);
}

void tickTaskStart(TickTask* t, TickTaskFn fn, u32 delay_ticks, u32 period_ticks)
{
	IrqState st = irqLock();

	if_unlikely (!s_tickInit) {
		tickInit();
	}

	t->target = tickGetCount() + delay_ticks;
	t->period = period_ticks;
	t->fn = fn;
	_tickTaskEnqueue(t);

	if (s_firstTask == t) {
		_tickTaskSchedule(t);
	}

	irqUnlock(st);
}

void tickTaskStop(TickTask* t)
{
	IrqState st = irqLock();

	if_unlikely (!t->fn) {
		irqUnlock(st);
		return;
	}

	bool need_resched = s_firstTask == t;
	_tickTaskDequeue(t);
	if (need_resched) {
		_tickTaskSchedule(s_firstTask);
	}

	t->fn = NULL;
	irqUnlock(st);
}
