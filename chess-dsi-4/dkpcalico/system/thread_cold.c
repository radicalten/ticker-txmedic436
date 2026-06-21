// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
// Avoid a hard dependency on _impure_data if it is never referenced
#define __ATTRIBUTE_IMPURE_DATA__ __attribute__((weak))

#include "thread-priv.h"

typedef struct TlsInfo {
	void*  start;
	size_t total_sz;
	size_t load_sz;
} TlsInfo;

extern TlsInfo __tls_info MK_WEAK;

#if defined(__GBA__) || (defined(__NDS__) && defined(ARM7))
void svcHalt(void);

//static alignas(8) u32 s_idleThreadStack[6];
// Using the bottommost 6 words of the IRQ stack as the idle thread stack
extern u32 __sp_usr[];
#define s_idleThreadStack __sp_usr
#endif

static Thread s_mainThread, s_idleThread;
static ThrListNode s_joinThreads, s_sleepThreads;

MK_INLINE void* _threadGetMainTp(void)
{
	if (&__tls_info) {
		return (char*)__tls_info.start - 2*sizeof(void*);
	} else {
		return NULL;
	}
}

static void _threadTickTask(TickTask* task)
{
	threadUnblockAllByValue(&s_sleepThreads, (u32)task);
}

void _threadInit(void)
{
	// Set up main thread (which is also the current one)
	s_firstThread          = &s_mainThread;
	s_curThread            = &s_mainThread;
	s_mainThread.tp        = _threadGetMainTp();
	s_mainThread.impure    = &_impure_data;
	s_mainThread.next      = &s_idleThread;
	s_mainThread.status    = ThrStatus_Running;
	s_mainThread.prio      = MAIN_THREAD_PRIO;
	s_mainThread.baseprio  = s_mainThread.prio;

	// Set up idle thread
	s_idleThread.ctx.psr   = ARM_PSR_MODE_SYS;
#if __ARM_ARCH >= 5
	s_idleThread.ctx.r[15] = (u32)armWaitForIrq;
	s_idleThread.ctx.r[14] = s_idleThread.ctx.r[15];
#elif defined(__GBA__) || defined(__NDS__)
	s_idleThread.ctx.r[14] = (u32)svcHalt;
	s_idleThread.ctx.r[15] = s_idleThread.ctx.r[14] - 1;
	s_idleThread.ctx.r[13] = (u32)&s_idleThreadStack[2];
	s_idleThread.ctx.sp_svc = (u32)&s_idleThreadStack[6];
	s_idleThread.ctx.psr  |= ARM_PSR_T;
#else
#error "This ARM7 platform is not yet supported"
#endif
	s_idleThread.tp        = s_mainThread.tp;
	s_idleThread.impure    = s_mainThread.impure;
	s_idleThread.status    = ThrStatus_Running;
	s_idleThread.prio      = THREAD_MIN_PRIO+1;
	s_idleThread.baseprio  = s_idleThread.prio;
}

void threadPrepare(Thread* t, ThreadFunc entrypoint, void* arg, void* stack_top, u8 prio)
{
	// Initialize thread state and context
	memset(t, 0, sizeof(Thread));
	t->ctx.r[0]   = (u32)arg;
	t->ctx.sp_svc = (u32)stack_top &~ 7;
	t->ctx.r[13]  = t->ctx.sp_svc - 0x10;
	t->ctx.r[14]  = (u32)threadExit;
	t->ctx.r[15]  = (u32)entrypoint;
	t->ctx.psr    = ARM_PSR_MODE_SYS;
	t->tp         = s_mainThread.tp;
	t->impure     = s_mainThread.impure;
	t->status     = ThrStatus_Waiting;
	t->prio       = prio & THREAD_MIN_PRIO;
	t->baseprio   = t->prio;
	t->pause      = 1;

	// Adjust THUMB entrypoints
	if (t->ctx.r[15] & 1) {
		t->ctx.r[15] &= ~1;
		t->ctx.psr   |= ARM_PSR_T;
	}

	// Insert into thread list
	ArmIrqState st = armIrqLockByPsr();
	threadEnqueue(t);
	armIrqUnlockByPsr(st);
}

size_t threadGetLocalStorageSize(void)
{
	size_t needed_sz = 0;
	if (&__tls_info) {
		needed_sz += __tls_info.total_sz;
	}
	if (&_impure_data) {
		needed_sz += sizeof(struct _reent);
	}
	return (needed_sz + 7) &~ 7;
}

void threadAttachLocalStorage(Thread* t, void* storage)
{
	// Retrieve needed storage size - return early if 0
	size_t needed_sz = threadGetLocalStorageSize();
	if (!needed_sz) {
		return;
	}

	// If storage wasn't passed, allocate reent struct from the thread's stack
	if (!storage) {
		t->ctx.r[13] -= needed_sz;
		storage = (void*)t->ctx.r[13];
	}

	// Zerofill storage
	armFillMem32(storage, 0, needed_sz);

	// Handle TLS segment if present
	if (&__tls_info) {
		// Attach thread pointer
		t->tp = (char*)storage - 2*sizeof(void*);

		// Copy initializer data if needed
		if (__tls_info.load_sz) {
			armCopyMem32(storage, __tls_info.start, __tls_info.load_sz);
		}
	}

	// Handle impure data if present
	if (&_impure_data) {
		// Attach reent struct to thread
		struct _reent* r = (struct _reent*)((char*)storage + needed_sz - sizeof(struct _reent));
		t->impure = r;

		// Inherit standard streams from current thread
		struct _reent* parent = (struct _reent*)threadGetSelf()->impure;
		__FILE* _stdin  = parent->_stdin;
		__FILE* _stdout = parent->_stdout;
		__FILE* _stderr = parent->_stderr;

		// Initialize the new reent struct
		_REENT_INIT_PTR_ZEROED(r);
		r->_stdin  = _stdin;
		r->_stdout = _stdout;
		r->_stderr = _stderr;
	}
}

void threadStart(Thread* t)
{
	Thread* self = s_curThread;
	ArmIrqState st = armIrqLockByPsr();

	if (!t->pause || (--t->pause) || t->queue) {
		armIrqUnlockByPsr(st);
		return;
	}

	t->status = ThrStatus_Running;

	if (t != self) {
		threadReschedule(t, st);
	} else {
		// We are assuming 1) we are in IRQ mode, 2) threadPause(self) was called before, and thus 3) s_deferredThread != NULL
		if (t->prio < s_deferredThread->prio) {
			s_deferredThread = NULL;
		}

		armIrqUnlockByPsr(st);
	}
}

void threadPause(Thread* t)
{
	Thread* self = s_curThread;
	ArmIrqState st = armIrqLockByPsr();

	if ((t->pause++) != 0 || t->status != ThrStatus_Running) {
		armIrqUnlockByPsr(st);
		return;
	}

	t->status = ThrStatus_Waiting;
	t->queue = NULL;

	Thread* next = NULL;
	if (t == self || t == s_deferredThread) {
		next = threadFindRunnable(s_firstThread);
	}

	if (t == self) {
		threadSwitchTo(next, st);
	} else {
		if (t == s_deferredThread) {
			s_deferredThread = next != self ? next : NULL;
		}

		armIrqUnlockByPsr(st);
	}
}

void threadSetPrio(Thread* t, u8 prio)
{
	Thread* self = s_curThread;
	ArmIrqState st = armIrqLockByPsr();

	unsigned curprio = self->prio;
	t->baseprio = prio & THREAD_MIN_PRIO;
	threadUpdateDynamicPrio(t);

	Thread* next = NULL;
	if (t == self) {
		if (self->prio > curprio) {
			next = threadFindRunnable(s_firstThread);
		}
	} else if (t->status == ThrStatus_Running) {
		next = t;
	}

	threadReschedule(next, st);
}

int threadJoin(Thread* t)
{
	ArmIrqState st = armIrqLockByPsr();

	// Block on thread if it's not already finished
	if (t->status >= ThrStatus_Running)
		threadBlock(&s_joinThreads, (u32)t);

	int rc = t->rc;

	armIrqUnlockByPsr(st);
	return rc;
}

void threadYield(void)
{
	Thread* self = s_curThread;
	ArmIrqState st = armIrqLockByPsr();

	Thread* t = threadFindRunnable(self->next);
	if (!t || t->prio > self->prio)
		t = threadFindRunnable(s_firstThread);

	if (t != self)
		threadSwitchTo(t, st);
	else
		armIrqUnlockByPsr(st);
}

MK_INLINE u32 _threadIrqWaitImpl(bool next_irq, IrqMask mask, ThrListNode* wait_list, IrqMask* wait_mask, volatile IrqMask* flags)
{
	if (!mask) return 0;
	ArmIrqState st = armIrqLockByPsr();

	IrqMask cur_flags = *flags;
	IrqMask test = cur_flags & mask;
	if (test) {
		*flags = cur_flags ^ test;
		if (!next_irq) {
			armIrqUnlockByPsr(st);
			return test;
		}
	}

	*wait_mask |= mask;
	u32 ret = threadBlock(wait_list, mask);
	armIrqUnlockByPsr(st);

	return ret;
}

u32 threadIrqWait(bool next_irq, IrqMask mask)
{
	return _threadIrqWaitImpl(next_irq, mask, &s_irqWaitList, &s_irqWaitMask, &__irq_flags);
}

#if MK_IRQ_NUM_HANDLERS > 32

u32 threadIrqWait2(bool next_irq, IrqMask mask)
{
	return _threadIrqWaitImpl(next_irq, mask, &s_irqWaitList2, &s_irqWaitMask2, &__irq_flags2);
}

#endif

void threadExit(int rc)
{
	Thread* self = s_curThread;
	armIrqLockByPsr();

	threadDequeue(self);
	self->status = ThrStatus_Finished;
	self->prio = THREAD_MAX_PRIO; // avoid preemption in threadUnblock
	self->rc = rc;
	threadUnblockAllByValue(&s_joinThreads, (u32)self);

	s_curThread = threadFindRunnable(s_firstThread);
	armContextLoad(&s_curThread->ctx);
}

void threadSleepTicks(u32 ticks)
{
	TickTask task;
	ArmIrqState st = armIrqLockByPsr();
	tickTaskStart(&task, _threadTickTask, ticks, 0);
	threadBlock(&s_sleepThreads, (u32)&task);
	armIrqUnlockByPsr(st);
}

void threadTimerStartTicks(TickTask* task, u32 period_ticks)
{
	tickTaskStart(task, _threadTickTask, period_ticks, period_ticks);
}

void threadTimerWait(TickTask* task)
{
	ArmIrqState st = armIrqLockByPsr();
	threadBlock(&s_sleepThreads, (u32)task);
	armIrqUnlockByPsr(st);
}
