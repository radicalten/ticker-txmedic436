// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/asm.inc>

.macro BIOSFUNC id, name
FUNC_START16 svc\name
	svc \id
	bx  lr
FUNC_END
.endm

@ "Basic" functions

@ 0x00: SoftResetNTR (does not exist on TWL)
@ 0x01: LZ77UncompWramCallbackTWL
@ 0x02: LZ77UncompVramCallbackTWL
BIOSFUNC 0x03, WaitByLoop
@ 0x04: IntrWait (bugged on TWL, do not use)
@ 0x05: VBlankIntrWait (bugged on TWL, do not use)
BIOSFUNC 0x06, Halt
#ifdef ARM7
BIOSFUNC 0x07, Sleep
BIOSFUNC 0x08, SoundBias
#endif
BIOSFUNC 0x09, DivMod
@ 0x0a: (unused)
BIOSFUNC 0x0b, CpuSet
@ 0x0c: CpuFastSet (bugged on both NTR and TWL)
BIOSFUNC 0x0d, Sqrt
BIOSFUNC 0x0e, GetCRC16
@ 0x0f: IsDebuggerNTR
BIOSFUNC 0x10, BitUnpack
BIOSFUNC 0x11, LZ77UncompWram
@ 0x12: LZ77UncompVramCallbackNTR
BIOSFUNC 0x13, HuffUncompCallback
BIOSFUNC 0x14, RLUncompWram
BIOSFUNC 0x15, RLUncompVramCallback
#ifdef ARM9
BIOSFUNC 0x16, Diff8bitUnfilterWram
@ 0x17: (unused)
BIOSFUNC 0x18, Diff16bitUnfilter
#endif
@ 0x19: Copy of LZ77UncompVramCallbackTWL
#ifdef ARM7
BIOSFUNC 0x1a, GetSineTable
@ 0x1b: GetPitchTable (bugged on TWL, workaround available)
BIOSFUNC 0x1c, GetVolumeTable
BIOSFUNC 0x1d, GetBootProcs
@ 0x1e: (unused)
@ 0x1f: CustomHalt (param in r2, see wrapper below)
#endif
#ifdef ARM9
BIOSFUNC 0x1f, CustomPost
#endif

FUNC_START16 svcLZ77UncompVramCallback
	push {r3}
	ldr  r3, =g_isTwlMode
	ldrb r3, [r3]
	cmp  r3, #0
	pop  {r3}
	beq  1f

	@ DSi BIOS
	svc  0x02
	bx   lr

	@ NDS BIOS
1:	svc  0x12
	bx   lr
FUNC_END

#ifdef ARM7

FUNC_START16 svcGetPitchTable
	ldr  r3, =g_isTwlMode
	ldrb r3, [r3]
	cmp  r3, #0
	beq  1f

	@ Account for bugged table offset (DSi BIOS workaround)
	ldr  r1, =0x46a
	sub  r0, r1

1:	svc  0x1b

	@ TODO: is casting to u16 really necessary?
	lsl  r0, #16
	lsr  r0, #16

	bx   lr
FUNC_END

FUNC_START16 svcCustomHalt
	mov  r2, r0
	svc  0x1f
	bx   lr
FUNC_END

#endif
