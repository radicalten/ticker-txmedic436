// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/irq.h>

extern IrqHandler __irq_table[MK_IRQ_NUM_HANDLERS];

MK_INLINE bool _irqMaskUnpack(IrqMask* pmask, unsigned* pid)
{
#if (__ARM_ARCH < 5) || __thumb__
	if (!*pmask)
		return false;
	while (!(*pmask & (1U << *pid)))
		++*pid;
#else
	int id = __builtin_ffs(*pmask)-1;
	if (id < 0)
		return false;
	*pid = id;
#endif
	*pmask &= ~(1U << *pid);
	return true;
}

void irqSet(IrqMask mask, IrqHandler handler)
{
	IrqState st = irqLock();
	unsigned id = 0;
	while (_irqMaskUnpack(&mask, &id))
		__irq_table[id] = handler;
	irqUnlock(st);
}

#if MK_IRQ_NUM_HANDLERS > 32

void irqSet2(IrqMask mask, IrqHandler handler)
{
	IrqState st = irqLock();
	unsigned id = 0;
	while (_irqMaskUnpack(&mask, &id))
		__irq_table[id+32] = handler;
	irqUnlock(st);
}

#endif
