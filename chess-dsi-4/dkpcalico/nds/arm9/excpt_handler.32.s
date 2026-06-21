// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/asm.inc>
#include <calico/arm/psr.h>
#include <calico/arm/cp15.h>
#include <calico/nds/mm_env.h>

SECT_TEXT __excpt_entry

@ Notes:
@ The least significant 2 bits of SP are used to provide additional information
@ about the exception.
@
@   sp.bit1 = Handler type
@             0 (BIOS) or 1 (calico)
@   sp.bit0 = Exception discriminator
@       BIOS: 0 (rst)  or 1 (pabt/dabt/und)
@     calico: 0 (dabt) or 1 (pabt/und)

@------------------------------------------------------------------------------
@ Reset (also occurs with NULL function pointers)
@------------------------------------------------------------------------------

.weak __arm_excpt_rst
.type __arm_excpt_rst, %function
__arm_excpt_rst:
	bkpt  #0x8000 @ cause a Prefetch Abort
	.size __arm_excpt_rst, .-__arm_excpt_rst

@------------------------------------------------------------------------------
@ Prefetch Abort
@------------------------------------------------------------------------------

.weak __arm_excpt_pabt
.type __arm_excpt_pabt, %function
__arm_excpt_pabt:
	sub   lr, lr, #4
	@ Fallthrough
	.size __arm_excpt_pabt, .-__arm_excpt_pabt

@------------------------------------------------------------------------------
@ Undefined Instruction
@------------------------------------------------------------------------------

.weak __arm_excpt_und
.type __arm_excpt_und, %function
__arm_excpt_und:
	ldr   sp, =MM_ENV_EXCPT_STACK_TOP
	orr   sp, sp, #3
	b     __excpt_entry
	.size __arm_excpt_und, .-__arm_excpt_und

@------------------------------------------------------------------------------
@ Data Abort
@------------------------------------------------------------------------------

.weak __arm_excpt_dabt
.type __arm_excpt_dabt, %function
__arm_excpt_dabt:
	sub   lr, lr, #8
	ldr   sp, =MM_ENV_EXCPT_STACK_TOP
	orr   sp, sp, #2
	@ Fallthrough
	.size __arm_excpt_dabt, .-__arm_excpt_dabt

@------------------------------------------------------------------------------

.type __excpt_entry, %function
__excpt_entry:
	@ Save r12 and pc (exception address)
	push  {r12, lr}

	@ Save cp15cr and exception spsr
	mrc   p15, 0, r12, c1, c0, 0
	mrs   lr, spsr
	push  {r12, lr}

	@ Disable MPU
	bic   r12, r12, #CP15_CR_PU_ENABLE
	mcr   p15, 0, r12, c1, c0, 0

	@ Invoke exception handler if registered
	bic   r12, sp, #3 @ remove extra flags
	ldr   r12, [r12, #0x10]
	cmp   r12, #0
	blxne r12

	@ Hang indefinitely if above returned
1:	b     1b

	.pool
	.size __excpt_entry, .-__excpt_entry
