// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/system/mutex.h>
#include <calico/system/mailbox.h>
#include <calico/nds/irq.h>
#include <calico/nds/pxi.h>
#include "transfer.h"

typedef struct PxiChannelState {
	void* user;
	PxiHandlerFn fn;
	u32 reply;
	Mutex recv_mutex;
} PxiChannelState;

static Mutex s_pxiSendMutex;
static ThrListNode s_pxiRecvQueue;
static u32 s_pxiRecvState;
static PxiChannelState s_pxiChannels[PxiChannel_Count];

MK_WEAK void _pxiRecvUnhandled(PxiChannel ch, u32 data)
{
}

MK_INLINE u32 _pxiProcessPacket(u32 packet)
{
	PxiChannel ch = pxiPacketGetChannel(packet);
	u32 imm, num_words;
	if_likely (ch != PxiChannel_Extended) {
		imm = pxiPacketGetImmediate(packet);
		num_words = 0;
	} else {
		ch = pxiExtPacketGetChannel(packet);
		imm = pxiExtPacketGetImmediate(packet);
		num_words = pxiExtPacketGetNumWords(packet);
		imm |= num_words << 26;
	}

	PxiChannelState* state = &s_pxiChannels[ch];

	if_likely (pxiPacketIsRequest(packet)) {
		if_likely (state->fn) {
			state->fn(state->user, imm);
			if_unlikely (num_words) {
				num_words |= (1U << 26) | (ch << 27);
			}
		} else {
			_pxiRecvUnhandled(ch, imm);
		}
	} else if_likely (state->recv_mutex.owner) {
		state->reply = imm;
		threadUnblockOneByValue(&s_pxiRecvQueue, ch);
	}

	return num_words;
}

MK_INLINE u32 _pxiProcessData(u32 state, u32 data)
{
	PxiChannel ch = (PxiChannel)(state >> 27);

	state --;
	if_unlikely (!(state & (1U << 26))) {
		_pxiRecvUnhandled(ch, data);
		return state;
	}

	PxiChannelState* st = &s_pxiChannels[ch];
	st->fn(st->user, data);

	if (!(state << 6)) {
		state = 0;
	}

	return state;
}

static void _pxiRecvIrqHandler(void)
{
	u32 state = s_pxiRecvState;

	while (!(REG_PXI_CNT & PXI_CNT_RECV_EMPTY)) {
		u32 data = REG_PXI_RECV;
		if_likely (!state) {
			state = _pxiProcessPacket(data);
		} else {
			state = _pxiProcessData(state, data);
		}
	}

	s_pxiRecvState = state;
}

static void _pxiMailboxHandler(void* user, u32 data)
{
	Mailbox* mb = (Mailbox*) user;
	mailboxTrySend(mb, data);
}

void _pxiInit(void)
{
	REG_PXI_CNT |= PXI_CNT_SEND_IRQ | PXI_CNT_RECV_IRQ;
	REG_PXI_SYNC = PXI_SYNC_IRQ_ENABLE;
	irqSet(IRQ_PXI_RECV, _pxiRecvIrqHandler);
	irqEnable(IRQ_PXI_SEND | IRQ_PXI_RECV | IRQ_PXI_SYNC);
}

void pxiWaitForPing(void)
{
	threadIrqWait(false, IRQ_PXI_SYNC);
}

void pxiSetHandler(PxiChannel ch, PxiHandlerFn fn, void* user)
{
	PxiChannelState* state = &s_pxiChannels[ch];
	IrqState st = irqLock();

	state->fn = fn;
	state->user = user;

	u32 old_mask = s_pxiLocalPxiMask, new_mask = old_mask;
	if (fn) {
		new_mask |= 1U << ch;
	} else {
		new_mask &= ~(1U << ch);
	}

	if (new_mask != old_mask) {
		s_pxiLocalPxiMask = new_mask;
		pxiPing();
	}

	irqUnlock(st);
}

void pxiSetMailbox(PxiChannel ch, struct Mailbox* mb)
{
	pxiSetHandler(ch, _pxiMailboxHandler, mb);
}

void pxiWaitRemote(PxiChannel ch)
{
	u32 mask = 1U << ch;
	while (!(s_pxiRemotePxiMask & mask)) {
		pxiWaitForPing();
	}
}

MK_INLINE void _pxiSendWord(u32 word)
{
	while (REG_PXI_CNT & PXI_CNT_SEND_FULL)
		threadIrqWait(false, IRQ_PXI_SEND);

	REG_PXI_SEND = word;
}

void pxiSendPacket(u32 packet)
{
	mutexLock(&s_pxiSendMutex);
	_pxiSendWord(packet);
	mutexUnlock(&s_pxiSendMutex);
}

void pxiSendExtPacket(u32 packet, const u32* data)
{
	mutexLock(&s_pxiSendMutex);

	u32 num_words = pxiExtPacketGetNumWords(packet);

	_pxiSendWord(packet);
	while (num_words--)
		_pxiSendWord(*data++);

	mutexUnlock(&s_pxiSendMutex);
}

void pxiBeginReceive(PxiChannel ch)
{
	PxiChannelState* state = &s_pxiChannels[ch];
	mutexLock(&state->recv_mutex);
	state->reply = PXI_NO_REPLY;
}

u32 pxiEndReceive(PxiChannel ch)
{
	PxiChannelState* state = &s_pxiChannels[ch];
	if (!mutexIsLockedByCurrentThread(&state->recv_mutex)) {
		return PXI_NO_REPLY;
	}

	ArmIrqState st = armIrqLockByPsr();

	if (state->reply == PXI_NO_REPLY) {
		threadBlock(&s_pxiRecvQueue, ch);
	}

	u32 reply = state->reply;
	state->reply = PXI_NO_REPLY;

	armIrqUnlockByPsr(st);

	mutexUnlock(&state->recv_mutex);

	return reply;
}
