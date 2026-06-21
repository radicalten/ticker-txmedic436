// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/system/mailbox.h>
#include <calico/nds/arm9/mic.h>
#include "../pxi/mic.h"

static struct {
	MicBufferFn fn;
	void* user;
	size_t byte_sz;
} s_micState;

static Thread s_micThread;
alignas(8) static u8 s_micThreadStack[0x2000];

MK_INLINE bool _micIssueCmd(PxiMicCmd cmd, unsigned imm)
{
	u32 msg = pxiMicMakeCmdMsg(cmd, imm);
	return pxiSendAndReceive(PxiChannel_Mic, msg) != 0;
}

static int _micThreadMain(void* unused)
{
	Mailbox mbox;
	u32 slots[2];
	mailboxPrepare(&mbox, slots, sizeof(slots)/sizeof(u32));
	pxiSetMailbox(PxiChannel_Mic, &mbox);

	for (;;) {
		void* buf = (void*)mailboxRecv(&mbox);
		if (s_micState.fn) {
			s_micState.fn(s_micState.user, buf, s_micState.byte_sz);
		}
	}

	return 0;
}

void micInit(void)
{
	pxiWaitRemote(PxiChannel_Mic);
}

bool micSetCpuTimer(unsigned prescaler, unsigned period)
{
	PxiMicImmDivTimer u = {
		.div   = prescaler,
		.timer = period,
	};

	return _micIssueCmd(PxiMicCmd_SetCpuTimer, u.imm);
}

bool micSetDmaRate(MicRate rate)
{
	PxiMicImmDivTimer u = {
		.div   = rate,
		.timer = 0,
	};

	return _micIssueCmd(PxiMicCmd_SetDmaRate, u.imm);
}

void micSetCallback(MicBufferFn fn, void* user)
{
	// Start mic thread if necessary
	if_unlikely (fn && !threadIsValid(&s_micThread)) {
		threadPrepare(&s_micThread, _micThreadMain, NULL, &s_micThreadStack[sizeof(s_micThreadStack)], 0x08);
		threadAttachLocalStorage(&s_micThread, NULL);
		threadStart(&s_micThread);
	}

	ArmIrqState st = armIrqLockByPsr();
	s_micState.fn = fn;
	s_micState.user = user;
	armIrqUnlockByPsr(st);
}

bool micStart(void* buf, size_t byte_sz, MicFmt fmt, MicMode mode)
{
	PxiMicArgStart arg = {
		.dest_addr = (u32)buf,
		.dest_sz   = byte_sz,
	};

	PxiMicImmStart u = {
		.is_16bit = fmt,
		.mode     = mode,
	};

	u32 msg = pxiMicMakeCmdMsg(PxiMicCmd_Start, u.imm);

	ArmIrqState st = armIrqLockByPsr();
	unsigned ret = pxiSendWithDataAndReceive(PxiChannel_Mic, msg, (u32*)&arg, sizeof(arg)/sizeof(u32));
	if (ret != 0) {
		s_micState.byte_sz = ret;
	}
	armIrqUnlockByPsr(st);

	return ret != 0;
}

void micStop(void)
{
	_micIssueCmd(PxiMicCmd_Stop, 0);
}
