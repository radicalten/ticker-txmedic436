// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/irq.h>
#include <calico/system/thread.h>
#include <calico/system/mailbox.h>
#include <calico/nds/timer.h>
#include <calico/nds/ndma.h>
#include <calico/nds/pxi.h>
#include <calico/nds/arm7/spi.h>
#include <calico/nds/arm7/tsc.h>
#include <calico/nds/arm7/codec.h>
#include <calico/nds/arm7/mic.h>
#include "../pxi/mic.h"

typedef union MicBuf {
	s16*  ptr16;
	s8*   ptr8;
	void* ptr;
} MicBuf;

static Thread s_micThread;
static alignas(8) u8 s_micThreadStack[0x200];

static struct {
	u16 latch;

	u16 active   : 1;
	u16 is_16bit : 1;
	u16 is_dma   : 1;
	u16 div      : 2;

	u16 timer;

	unsigned (* sample_fn)(void);

	MicBuf front;
	MicBuf back;

	unsigned pos;
	unsigned len;

	Mailbox mbox;
	u32 param[2];
} s_micState;

static unsigned _micReadSampleNtr(void)
{
	unsigned ret;
	if_likely (!g_spiMutex.owner) {
		ret = (tscReadChannel12(TscChannel_AUX) << 4) ^ 0x8000;
		s_micState.latch = ret;
	} else {
		ret = s_micState.latch;
	}

	return ret;
}

MK_INLINE void _micexStart(unsigned div, unsigned cnt)
{
	REG_MICEX_CNT = MICEX_CNT_CLEAR_FIFO;
	REG_MICEX_CNT = cnt | MICEX_CNT_RATE_DIV(div) | MICEX_CNT_ENABLE;
}

__attribute__((section(".twl._micReadSampleTwl")))
static unsigned _micReadSampleTwl(void)
{
	unsigned ret;
	if_likely (!(REG_MICEX_CNT & MICEX_CNT_FIFO_EMPTY)) {
		ret = REG_MICEX_DATA & 0xffff;
		s_micState.latch = ret;

		REG_MICEX_CNT = 0;
		_micexStart(0, 0);
	} else {
		ret = s_micState.latch;
	}

	return ret;
}

static void _micTimerIsr(void)
{
	unsigned sample = s_micState.sample_fn();
	unsigned pos = s_micState.pos;
	if_likely (s_micState.is_16bit) {
		s_micState.front.ptr16[pos++] = sample;
	} else {
		s_micState.front.ptr8[pos++] = sample >> 8;
	}

	if_likely (pos < s_micState.len) {
		s_micState.pos = pos;
		return;
	}

	s_micState.pos = 0;

	MicBuf tmp = s_micState.front;
	s_micState.front = s_micState.back;
	s_micState.back = tmp;

	if (!s_micState.front.ptr) {
		REG_IE &= ~IRQ_TIMER0;
		REG_TMxCNT_H(0) = 0;
	}

	mailboxTrySend(&s_micState.mbox, (1U<<31) | (uptr)tmp.ptr);
}

__attribute__((section(".twl._micDmaIsr")))
static void _micDmaIsr(void)
{
	MicBuf tmp = s_micState.front;
	s_micState.front = s_micState.back;
	s_micState.back = tmp;

	if (s_micState.front.ptr) {
		REG_NDMAxDAD(0) = (u32)s_micState.front.ptr;
		REG_NDMAxCNT(0) |= NDMA_START;
	} else {
		REG_IE &= ~IRQ_NDMA0;
		REG_MICEX_CNT = 0;
	}

	mailboxTrySend(&s_micState.mbox, (1U<<31) | (uptr)tmp.ptr);
}

static void _micPxiHandler(void* user, u32 data)
{
	static unsigned data_words = 0;
	static u32 header = 0;

	if_likely (data_words == 0) {
		data_words = data >> 26;
		if_likely (data_words == 0) {
			mailboxTrySend(&s_micState.mbox, data);
			return;
		}

		header = data & ((1U<<26)-1);
		return;
	}

	s_micState.param[sizeof(s_micState.param)/4 - (data_words--)] = data;
	if (data_words == 0) {
		mailboxTrySend(&s_micState.mbox, header);
	}
}

static bool _micSetCpuTimer(unsigned div, unsigned timer)
{
	if (s_micState.active) {
		return false;
	}

	s_micState.is_dma = false;
	s_micState.div = div;
	s_micState.timer = timer;
	return true;
}

static bool _micSetDmaRate(unsigned div)
{
	if (s_micState.active) {
		return false;
	}

	s_micState.is_dma = true;
	s_micState.div = div;
	return true;
}

static unsigned _micStart(bool is_16bit, MicMode mode)
{
	if (s_micState.active) {
		return 0;
	}

	PxiMicArgStart* arg = (PxiMicArgStart*)&s_micState.param[(sizeof(s_micState.param)-sizeof(PxiMicArgStart))/sizeof(u32)];
	u32 buf_addr = arg->dest_addr;
	u32 buf_sz = arg->dest_sz;

	if (s_micState.is_dma) {
		buf_addr &= ~3;
		buf_sz &= ~(8*4-1); // 8-word alignment
	} else if (is_16bit) {
		buf_addr &= ~1;
		buf_sz &= ~1;
	}

	if (!buf_sz) {
		return 0;
	}

	s_micState.front.ptr = (void*)buf_addr;
	switch (mode) {
		default:
		case MicMode_OneShot:
			s_micState.back.ptr = NULL;
			break;

		case MicMode_Repeat:
			s_micState.back = s_micState.front;
			break;

		case MicMode_DoubleBuffer:
			s_micState.back.ptr = &s_micState.front.ptr8[buf_sz];
			break;
	}

	s_micState.latch = 0;
	if (s_micState.is_dma) {
		REG_MICEX_CNT = 0;

		REG_NDMAxSAD(0) = (u32)&REG_MICEX_DATA;
		REG_NDMAxDAD(0) = (u32)s_micState.front.ptr;
		REG_NDMAxBCNT(0) = 0;
		REG_NDMAxTCNT(0) = buf_sz/4;
		REG_NDMAxWCNT(0) = 8;
		REG_NDMAxCNT(0) =
			NDMA_DST_MODE(NdmaMode_Increment) |
			NDMA_SRC_MODE(NdmaMode_Fixed) |
			NDMA_BLK_WORDS(8) |
			NDMA_TIMING(NdmaTiming_MicData) |
			NDMA_TX_MODE(NdmaTxMode_Timing) |
			NDMA_IRQ_ENABLE |
			NDMA_START;

		// XX: Consider adding a mic full ISR so that we can recover from overruns
		irqSet(IRQ_NDMA0, _micDmaIsr);
		irqEnable(IRQ_NDMA0);

		_micexStart(s_micState.div, MICEX_CNT_NO_R);
	} else {
		bool is_twl = cdcIsTwlMode();

		REG_TMxCNT_H(0) = 0;
		REG_TMxCNT_L(0) = 0x10000 - s_micState.timer;

		s_micState.sample_fn = is_twl ? _micReadSampleTwl : _micReadSampleNtr;
		s_micState.pos = 0;
		s_micState.len = buf_sz >> is_16bit;
		s_micState.is_16bit = is_16bit;

		irqSet(IRQ_TIMER0, _micTimerIsr);
		irqEnable(IRQ_TIMER0);

		if (is_twl) {
			REG_MICEX_CNT = 0;
			_micexStart(0, 0); // Use "fake stereo" mode so that 1 sample = 1 word
		}

		REG_TMxCNT_H(0) = s_micState.div | TIMER_ENABLE_IRQ | TIMER_ENABLE;
	}

	s_micState.active = true;
	return buf_sz;
}

static void _micStop(void)
{
	ArmIrqState st = armIrqLockByPsr();

	if (!s_micState.active) {
		armIrqUnlockByPsr(st);
		return;
	}

	if (s_micState.is_dma) {
		REG_IE &= ~IRQ_NDMA0;
		REG_MICEX_CNT = 0;
		REG_NDMAxCNT(0) = 0;
	} else {
		REG_IE &= ~IRQ_TIMER0;
		REG_TMxCNT_H(0) = 0;
	}

	s_micState.active = false;
	armIrqUnlockByPsr(st);
}

static int _micThreadMain(void* unused)
{
	u32 slots[4];
	mailboxPrepare(&s_micState.mbox, slots, sizeof(slots)/sizeof(slots[0]));

	pxiSetHandler(PxiChannel_Mic, _micPxiHandler, NULL);

	for (;;) {
		u32 msg = mailboxRecv(&s_micState.mbox);
		if (msg & (1U<<31)) {
			pxiSend(PxiChannel_Mic, msg);
			continue;
		}

		unsigned ret = 0;
		unsigned imm = pxiMicCmdGetImm(msg);
		switch (pxiMicCmdGetType(msg)) {
			default: break;

			case PxiMicCmd_SetCpuTimer: {
				PxiMicImmDivTimer u = { imm };
				ret = _micSetCpuTimer(u.div, u.timer);
				break;
			}

			case PxiMicCmd_SetDmaRate: {
				PxiMicImmDivTimer u = { imm };
				if (cdcIsTwlMode()) {
					ret = _micSetDmaRate(u.div);
				} else {
					// SOUND_MIXER_FREQ_HZ = SYSTEM_CLOCK/1024
					ret = _micSetCpuTimer(TIMER_PRESCALER_1024, 1+u.div);
				}
				break;
			}

			case PxiMicCmd_Start: {
				PxiMicImmStart u = { imm };
				ret = _micStart(u.is_16bit, (MicMode)u.mode);
				break;
			}

			case PxiMicCmd_Stop: {
				_micStop();
				break;
			}
		}

		pxiReply(PxiChannel_Mic, ret);
	}

	return 0;
}

__attribute__((target("thumb")))
void micStartServer(u8 thread_prio)
{
	threadPrepare(&s_micThread, _micThreadMain, NULL, &s_micThreadStack[sizeof(s_micThreadStack)], thread_prio);
	threadStart(&s_micThread);
}
