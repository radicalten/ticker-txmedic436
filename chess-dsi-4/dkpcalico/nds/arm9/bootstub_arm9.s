// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/asm.inc>
#include <calico/arm/psr.h>
#include <calico/arm/cp15.h>
#include <calico/nds/mm.h>
#include <calico/nds/mm_env.h>
#include <calico/nds/io.h>

//#define COLOR_DEBUG

REFERENCE __newlib_syscalls

.section .vectors, "ax", %progbits
.align 2
	b .Lrst
	b .Lund
	b .Lsvc
	b .Lpabt
	b .Ldabt
	bkpt #0x8014
	b .Lirq
	bkpt #0x801c

.section .itcm, "ax", %progbits
.align 2
.Lrst:  ldr pc, .Lvec_rst
.Lund:  ldr pc, .Lvec_und
.Lsvc:  ldr pc, .Lvec_svc
.Lpabt: ldr pc, .Lvec_pabt
.Ldabt: ldr pc, .Lvec_dabt
.Lirq:  ldr pc, .Lvec_irq

.global __excpt_vectors
__excpt_vectors:
.Lvec_rst:  .word __arm_excpt_rst
.Lvec_und:  .word __arm_excpt_und
.Lvec_svc:  .word __arm_excpt_svc
.Lvec_pabt: .word __arm_excpt_pabt
.Lvec_dabt: .word __arm_excpt_dabt
.Lvec_irq:  .word __arm_excpt_irq
.size __excpt_vectors, .-__excpt_vectors

FUNC_START32 __ds9_bootstub, bootstub
	b .Lactual_start

.Lmodule_header:
	.ascii "MOD9"
	.hword 1 @ Flags
	.hword .Lactual_start - .Lmodule_header

	.word __loadlist_lma
	.word __loadlist_start
	.word __loadlist_end
	.word __twl_loadlist_lma
	.word __twl_loadlist_start
	.word __twl_loadlist_end

	.word 0 @ Main thread stack size override (0=disable)
	.word __dldi_lma

.Lactual_start:
	@ Switch to supervisor mode and mask interrupts
	msr  cpsr_c, #(ARM_PSR_I | ARM_PSR_F | ARM_PSR_MODE_SVC)

	@ Disable everything in control register
	ldr  r12, =CP15_CR_SB1 | CP15_CR_ALT_VECTORS
	mcr  p15, 0, r12, c1, c0, 0

	@ Invalidate instruction/data caches, and drain write buffer
	mov  r0, #0
	mcr  p15, 0, r0, c7, c5, 0  @ ICache
	mcr  p15, 0, r0, c7, c6, 0  @ DCache
	mcr  p15, 0, r0, c7, c10, 4 @ Write buffer

	@ Configure 16K DTCM
	ldr  r0, =__dtcm_start
	orr  r0, r0, #CP15_TCM_16K
	mcr  p15, 0, r0, c9, c1, 0

	@ Configure 32K ITCM (mirrored throughout 0x0000000..0x1FFFFFF)
	mov  r0, #CP15_TCM_32M
	mcr  p15, 0, r0, c9, c1, 1

	@ Enable DTCM/ITCM
	orr  r12, r12, #(CP15_CR_DTCM_ENABLE | CP15_CR_ITCM_ENABLE)
	mcr  p15, 0, r12, c1, c0, 0

	@ Set up supervisor mode stack
	ldr  sp, =__sp_svc

	@ Disable interrupts in the IRQ controller
	mov  r11, #MM_IO
	strb r11, [r11, #IO_IME]

	@ Set up interrupt mode stack
	msr  cpsr_c, #(ARM_PSR_I | ARM_PSR_F | ARM_PSR_MODE_IRQ)
	ldr  sp, =__sp_irq

	@ Set up system mode stack (and unmask interrupts)
	msr  cpsr_c, #ARM_PSR_MODE_SYS
	ldr  sp, =__sp_usr

	@ Check if we are running in DSi mode
	ldr   r4, =MM_IO + IO_SCFG_ROM
	ldr   r4, [r4]
	and   r4, r4, #3
	cmp   r4, #1
	movne r4, #0

#ifdef COLOR_DEBUG
	@ MASTER_BRIGHT(_SUB) = 0
	mov  r1, #0
	strh r1, [r11, #IO_MASTER_BRIGHT]
	add  r10, r11, #IO_GFX_B
	strh r1, [r10, #IO_MASTER_BRIGHT]

	@ DISPCNT(_SUB) = MODE_GRAPHICS
	mov  r0, #0x10000
	str  r0, [r11, #IO_DISPCNT]
	str  r0, [r10, #IO_DISPCNT]

	@ Disable all VRAM banks
	add  r9, r11, #IO_VRAMCNT_A
	str  r1, [r9, #0] @ ABCD
	strh r1, [r9, #4] @ EF
	strb r1, [r9, #6] @ G
	strh r1, [r9, #8] @ HI

	@ Set both screens to red
	mov  r8, #MM_PALRAM
	mov  r0, #0x1f @ red
	str  r0, [r8, #0]
	str  r0, [r8, #0x400]
#endif

	@ Synchronize with arm7, sending DSi mode flag and waiting for it to finish running crt0
	adr  r0, .LsyncWith7
	ldr  r1, =MM_ITCM
	mov  r2, #.LsyncWith7_end-.LsyncWith7
1:	ldr  r3, [r0], #4
	str  r3, [r1], #4
	subs r2, r2, #4
	bne  1b
	ldr  r3, =(1<<3) | (1<<14) | (1<<15)
	ldr  r0, =MM_ITCM
	ldr  r5, =MM_IO + IO_SCFG_CLK
	blx  r0

#ifdef COLOR_DEBUG
	@ Set both screens to green
	mov  r0, #0x1f<<5 @ green
	str  r0, [r8, #0]
	str  r0, [r8, #0x400]
#endif

	@ Copy crt0 section to the correct address
	adr  r12, .Lcrt0_load
	ldm  r12, {r0-r2}
.Lcopy_crt0: @ Linkscript guarantees crt0 size is 32-byte aligned
	ldm  r0!, {r3,r5,r6,r7,r9,r11,r12,r14}
	stm  r1!, {r3,r5,r6,r7,r9,r11,r12,r14}
	cmp  r1, r2
	bne  .Lcopy_crt0

	@ Run the crt0 logic
	adr  r0, .Lmodule_header
	mov  r1, r4
	mov  r2, r10
	ldr  lr, =__ds9_bootstub_trampoline
	ldr  pc, =crt0Startup

.Lcrt0_load:
	.word __crt0_lma
	.word __crt0_start
	.word __crt0_end

.LsyncWith7:
	str  r3, [r11, #IO_PXI_CNT]
	ldrb r0, [r11, #IO_PXI_SYNC]
	mov  r1, #0
1:	add  r1, r1, #1
	and  r1, r1, #0xF
	strb r1, [r11, #IO_PXI_SYNC+1]
	ldrb r2, [r11, #IO_PXI_SYNC]
	cmp  r0, r2
	beq  1b
	add  r1, r1, #1
	and  r1, r1, #0xF
	strb r1, [r11, #IO_PXI_SYNC+1]

	@ Send DSi mode flag
	str  r4, [r11, #IO_PXI_SEND]

	@ Wait for the A-OK from ARM7
1:	ldr  r0, [r11, #IO_PXI_CNT]
	tst  r0, #(1<<8)
	bne  1b

	@ Read message from ARM7
	mov  r10, #MM_IO + IO_PXI_RECV
	ldr  r10, [r10]

	@ If on DSi: check if 134MHz mode is enabled
	cmp  r4, #0
	bxeq lr @ not DSi
	ldrh r1, [r5]
	tst  r1, #1
	bxne lr @ already enabled

	@ Enable 134MHz mode
	orr  r1, r1, #1
	mov  r2, #8
	strh r1, [r5]
1:	subs r2, r2, #1
	bge  1b

	bx   lr
.LsyncWith7_end:

FUNC_END

FUNC_START32 __ds9_bootstub_trampoline, crt0, local

#ifdef COLOR_DEBUG
	mov  r0, #0x1f<<10
	str  r0, [r8, #0]
	str  r0, [r8, #0x400]
#endif

	@ Check if the requested main thread stack size fits in DTCM
	ldr  r0, =__stacksize__
	ldr  r1, =__dtcm_bss_end
	ldr  r0, [r0]
	sub  r1, sp, r1
	cmp  r0, r1
	bls  1f

	@ If not: carve out main thread stack from heap
	ldr  r1, =fake_heap_start
	ldr  r2, [r1]
	add  r2, r2, r0
	add  r2, r2, #7
	bic  sp, r2, #7
	str  sp, [r1]

1:
	@ Call global constructors
	bl   __libc_init_array

	@ Jump to main()
	ldr  r2, =MM_ENV_ARGV_HEADER+0x0c
	ldr  r4, =main
	ldr  lr, =exit
	ldm  r2, {r0, r1} @ argc, argv
	bx   r4

FUNC_END
