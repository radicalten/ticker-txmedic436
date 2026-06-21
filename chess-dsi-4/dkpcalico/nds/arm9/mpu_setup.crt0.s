// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/asm.inc>
#include <calico/arm/cp15.h>
#include <calico/nds/mm.h>

FUNC_START32 crt0SetupMPU

	push  {r4-r11}

	adr   r3, .LmpuRegions
	cmp   r0, #0
	ldm   r3, {r4-r11}

	orrne r5, r5, #CP15_PU_16M
	bne   1f
	svc   0x0f0000 @ svcIsDebugger
	cmp   r0, #0
	orreq r5, r5, #CP15_PU_4M
	orrne r5, r5, #CP15_PU_8M
1:

	mcr   p15, 0, r4,  c6, c0, 0
	mcr   p15, 0, r5,  c6, c1, 0
	mcr   p15, 0, r6,  c6, c2, 0
	mcr   p15, 0, r7,  c6, c3, 0
	mcr   p15, 0, r8,  c6, c4, 0
	mcr   p15, 0, r9,  c6, c5, 0
	mcr   p15, 0, r10, c6, c6, 0
	mcr   p15, 0, r11, c6, c7, 0

	mov   r3, #0b00000010
	mcr   p15, 0, r3, c3, c0, 0
	mov   r3, #0b01000010
	mcr   p15, 0, r3, c2, c0, 0
	mcr   p15, 0, r3, c2, c0, 1
	ldr   r3, =0x66600060
	mcr   p15, 0, r3, c5, c0, 3
	ldr   r3, =0x06330033
	mcr   p15, 0, r3, c5, c0, 2

	mrc   p15, 0, r3, c1, c0, 0
	ldr   r2, =(CP15_CR_PU_ENABLE | CP15_CR_DCACHE_ENABLE | CP15_CR_ICACHE_ENABLE | CP15_CR_ROUND_ROBIN)
	orr   r3, r3, r2
	bic   r3, r3, #CP15_CR_ALT_VECTORS
	mcr   p15, 0, r3, c1, c0, 0

	pop   {r4-r11}
	bx    lr

.LmpuRegions:
	.word CP15_PU_ENABLE | CP15_PU_64M | MM_IO   @ IO + VRAM
	.word CP15_PU_ENABLE | MM_MAINRAM            @ Main RAM
	.word 0
	.word 0
	.word CP15_PU_ENABLE | CP15_PU_64K | MM_DTCM @ DTCM + high shared memory
	.word CP15_PU_ENABLE | CP15_PU_32K | MM_ITCM @ ITCM
	.word CP15_PU_ENABLE | CP15_PU_32K | MM_BIOS @ BIOS
	.word CP15_PU_ENABLE | CP15_PU_4K            @ Exception vectors (in ITCM)

FUNC_END
