// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/asm.inc>
#include <calico/arm/psr.h>
#include <calico/nds/mm.h>
#include <calico/nds/io.h>

#if defined(ARM9)

FUNC_START32 __arm_excpt_irq

	sub   lr, lr, #4
	push  {r0-r3,r12,lr}

#elif defined(ARM7)

FUNC_START32 __irq_handler

#endif

	@ Retrieve active interrupt mask
	ldr   r12, =MM_IO + IO_IE
	ldm   r12, {r0, r1}       @ read both IE and IF at the same time
	ands  r0, r0, r1          @ r0 <- IO_IE & IO_IF
#if defined(ARM9)
	ldmeqia sp!, {r0-r3,r12,pc}^ @ GAS doesn't like pop {reglist}^; so use the "real" opcode instead
#elif defined(ARM7)
	bne   1f

	@ Try the second IRQ controller
	add   r12, r12, #IO_IE2-IO_IE
	ldm   r12, {r0, r1}
	ands  r0, r0, r1
	bxeq  lr
1:
#endif

	@ Select an interrupt (LSB has priority)
#if defined(ARM9)
	neg   r1, r0
	and   r1, r1, r0
	clz   r2, r1
	mov   r3, #1
	rsb   r1, r2, #31
	mov   r2, r3, lsl r1

	ldr   r3, =__irq_flags
#elif defined(ARM7)
	mov   r3, #1              @ dummy 1 needed for shifting
	mov   r1, #0              @ bit counter
1:	ands  r2, r0, r3, lsl r1  @ r2 -> cur_irq_mask
	addeq r1, r1, #1          @ r1 -> cur_irq_id
	beq   1b

	@ Adjust for the second IRQ controller
	tst   r12, #8
	ldreq r3, =__irq_flags
	ldrne r3, =__irq_flags2
	addne r1, r1, #32
#endif

	@ Acknowledge the interrupt
	str   r2, [r12, #IO_IF-IO_IE]

	@ __irq_flags{2} |= cur_irq_mask
	ldr   r0, [r3]
	orr   r0, r0, r2
	str   r0, [r3]

	@ Load the handler and call it
	ldr   r3, =__irq_table
	ldr   r3, [r3, r1, lsl #2]
#if defined(ARM7)
	push  {r1, lr} @ save irq_id & BIOS return address
#elif defined(ARM9)
	mcr   p15, 0, r2, c13, c0, 1 @ save irq_mask abusing CP15 "Trace Process ID" to shave off stack usage
#endif
	cmp   r3, #0
#if defined(ARM7)
	adr   lr, 1f
	moveq r3, lr @ avoid crashing if no handler is registered
	bx    r3
1:
#elif defined(ARM9)
	blxne r3
#endif

	@ Check if thread rescheduling is needed
#if defined(ARM7)
	ldr   r0, [sp, #0]       @ r0 <- cur_irq_id
	mov   r1, #1
	subs  r2, r0, #32
	bhs   .LcheckIrqWait2
	mov   r3, r1, lsl r0     @ r3 <- cur_irq_mask
#elif defined(ARM9)
	mrc   p15, 0, r3, c13, c0, 1 @ r3 <- cur_irq_mask
#endif
	ldr   r2, =__sched_state
	ldr   r1, [r2, #3*4]     @ r1 <- s_irqWaitMask
	ands  r3, r3, r1         @ cur_irq_mask &= s_irqWaitMask
	beq   .LcheckReschedule  @ if (!cur_irq_mask) -> skip this section

	@ s_irqWaitMask &= ~cur_irq_mask
	bic   r1, r1, r3
	str   r1, [r2, #3*4]

	@ __irq_flags &= ~cur_irq_mask
	ldr   r12, =__irq_flags
	ldr   r1, [r12]
	bic   r1, r1, r3
	str   r1, [r12]

	@ threadUnblock(&irqWaitList, -1, ThrUnblockMode_ByMask, cur_irq_mask)
	add   r0, r2, #4*4
.LcontUnblockIrqWaitThreads:
	mov   r1, r3
	bl    threadUnblockAllByMask

	@ Check if we have a pending reschedule
	ldr   r2, =__sched_state
.LcheckReschedule:
#if defined(ARM7)
	add   sp, sp, #8
#endif
	ldr   r0, [r2, #4] @ r0 <- s_deferredThread
	cmp   r0, #0
#if defined(ARM7)
	ldreq pc, [sp, #-4] @ Return to BIOS if not
#elif defined(ARM9)
	ldmeqia sp!, {r0-r3,r12,pc}^
#endif

	@ s_curThread = s_deferredThread; s_deferredThread = NULL;
	mov   r3, #0
	ldr   r1, [r2, #0]
	stm   r2, {r0, r3}

	@ Save old thread's context
	mrs  r2, spsr
	str  r2, [r1, #16*4]
	pop  {r2,r3}
	stm  r1!, {r2,r3}
	pop  {r2,r3,r12,lr}
#if defined(ARM7)
	sub  lr, lr, #4
#endif
	stm  r1, {r2-r14}^
	str  lr, [r1, #(15-2)*4]
	msr  cpsr_c, #(ARM_PSR_I | ARM_PSR_F | ARM_PSR_MODE_SVC)
	str  sp, [r1, #(17-2)*4]

	@ Load new thread's context
	b    armContextLoadFromSvc

#if defined(ARM7)
.LcheckIrqWait2:
	@ As above, but for the second IRQ controller
	mov   r3, r1, lsl r2     @ r3 <- cur_irq_mask
	ldr   r2, =__sched_state
	ldr   r1, [r2, #6*4]     @ r1 <- s_irqWaitMask2
	ands  r3, r3, r1         @ cur_irq_mask &= s_irqWaitMask2
	beq   .LcheckReschedule  @ if (!cur_irq_mask) -> skip this section

	@ s_irqWaitMask2 &= ~cur_irq_mask
	bic   r1, r1, r3
	str   r1, [r2, #6*4]

	@ __irq_flags2 &= ~cur_irq_mask
	ldr   r12, =__irq_flags2
	ldr   r1, [r12]
	bic   r1, r1, r3
	str   r1, [r12]

	@ threadUnblock(&irqWaitList2, -1, ThrUnblockMode_ByMask, cur_irq_mask)
	add   r0, r2, #7*4
	b     .LcontUnblockIrqWaitThreads
#endif

FUNC_END
