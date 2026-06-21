// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/asm.inc>
#include <calico/arm/psr.h>
#include <calico/nds/mm.h>
#include <calico/nds/mm_env.h>
#include <calico/nds/io.h>

REFERENCE __newlib_syscalls

FUNC_START32 __ds7_bootstub, bootstub
	b .Lactual_start

.Lmodule_header:
	.ascii "MOD7"
	.hword 1 @ Flags
	.hword .Lactual_start - .Lmodule_header

	.word __loadlist_lma
	.word __loadlist_start
	.word __loadlist_end
	.word __twl_loadlist_lma
	.word __twl_loadlist_start
	.word __twl_loadlist_end

.Lactual_start:
	@ Switch to supervisor mode and mask interrupts
	msr  cpsr_c, #(ARM_PSR_I | ARM_PSR_F | ARM_PSR_MODE_SVC)

	@ Set up supervisor mode stack
	ldr  sp, =__sp_svc

	@ Disable interrupts in the IRQ controller
	mov  r11, #MM_IO
	strb r11, [r11, #IO_IME]

	@ Set interrupt vector
	ldr  r12, =__irq_vector
	ldr  r1,  =__irq_handler
	str  r1, [r12]

	@ Set up interrupt mode stack
	msr  cpsr_c, #(ARM_PSR_I | ARM_PSR_F | ARM_PSR_MODE_IRQ)
	ldr  sp, =__sp_irq

	@ Set up system mode stack (and unmask interrupts)
	msr  cpsr_c, #ARM_PSR_MODE_SYS
	ldr  sp, =__sp_usr

	@ Synchronize with arm9, receiving DSi mode flag
	adr  r0, .LsyncWith9
	sub  r1, sp, #.LsyncWith9_end-.LsyncWith9
1:	ldr  r2, [r0], #4
	str  r2, [r1], #4
	cmp  r1, sp
	bne  1b
	adr  lr, 1f
	ldr  r3, =(1<<3) | (1<<14) | (1<<15)
	sub  pc, sp, #.LsyncWith9_end-.LsyncWith9
1:

	@ Run the crt0 logic
	adr  r0, .Lmodule_header
	mov  r1, r4
	ldr  r2, =crt0Startup
	adr  lr, 1f
	bx   r2
1:
	@ Call global constructors
	ldr  r0, =__libc_init_array
	adr  lr, 1f
	bx   r0
1:
	@ Calculate end of main RAM reserved for ARM9
	cmp  r4, #0
	ldr  r4, =__main_start
	bne  1f
	sub  r4, r4, #(MM_MAINRAM_SZ_TWL-MM_MAINRAM_SZ_NTR)
	svc  0x0f0000 @ svcIsDebugger
	cmp  r0, #0
	addne r4, r4, #(MM_MAINRAM_SZ_DBG-MM_MAINRAM_SZ_NTR)
1:
	@ Set up trampoline for jumping to main
	adr  r7, .LjumpToMain
	ldm  r7, {r8-r9}
	stmdb sp, {r8-r9}

	@ Jump to main()
	ldr  r2, =MM_ENV_ARGV_HEADER+0x0c
	ldr  r5, =main
	ldr  lr, =exit
	ldm  r2, {r0, r1} @ argc, argv
	sub  pc, sp, #8

.LsyncWith9:
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

	@ Wait for DSi mode flag to arrive
1:	ldr  r0, [r11, #IO_PXI_CNT]
	tst  r0, #(1<<8)
	bne  1b

	@ Read DSi mode flag
	mov  r4, #MM_IO + IO_PXI_RECV
	ldr  r4, [r4]

	bx   lr
.LsyncWith9_end:

.LjumpToMain:
	str  r4, [r11, #IO_PXI_SEND]
	bx   r5

FUNC_END
