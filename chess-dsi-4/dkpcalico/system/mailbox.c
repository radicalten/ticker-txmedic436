// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/arm/common.h>
#include <calico/system/thread.h>
#include <calico/system/mailbox.h>

static ThrListNode s_mailboxRecvQueue;

bool mailboxTrySend(Mailbox* mb, u32 message)
{
	ArmIrqState st = armIrqLockByPsr();

	if_unlikely (mb->pending_slots == mb->num_slots) {
		armIrqUnlockByPsr(st);
		return false;
	}

	unsigned next_slot = mb->cur_slot + mb->pending_slots++;
	if (next_slot >= mb->num_slots) {
		next_slot -= mb->num_slots;
	}

	mb->slots[next_slot] = message;
	if_likely (mb->recv_waiters) {
		mb->recv_waiters --;
		threadUnblockOneByValue(&s_mailboxRecvQueue, (u32)mb);
	}

	armIrqUnlockByPsr(st);
	return true;
}

bool mailboxTryRecv(Mailbox* mb, u32* out)
{
	ArmIrqState st = armIrqLockByPsr();

	if_unlikely (!mb->pending_slots) {
		armIrqUnlockByPsr(st);
		return false;
	}

	*out = mb->slots[mb->cur_slot++];
	mb->pending_slots --;
	if (mb->cur_slot >= mb->num_slots) {
		mb->cur_slot -= mb->num_slots;
	}

	armIrqUnlockByPsr(st);
	return true;
}

u32 mailboxRecv(Mailbox* mb)
{
	ArmIrqState st = armIrqLockByPsr();

	if_unlikely (!mb->pending_slots) {
		mb->recv_waiters ++;
		threadBlock(&s_mailboxRecvQueue, (u32)mb);
	}

	u32 message = mb->slots[mb->cur_slot++];
	mb->pending_slots --;
	if (mb->cur_slot >= mb->num_slots) {
		mb->cur_slot -= mb->num_slots;
	}

	armIrqUnlockByPsr(st);
	return message;
}
