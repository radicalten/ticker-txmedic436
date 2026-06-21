// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/asm.inc>

.macro BIOSFUNC id, name
FUNC_START16 svc\name
	svc \id
	bx  lr
FUNC_END
.endm

BIOSFUNC 0x01, LZ77UncompWramCallbackTWL
@ 0x02: LZ77UncompVramCallbackTWL (not here because it's wrapped)
@ 0x19: Copy of LZ77UncompVramCallbackTWL

BIOSFUNC 0x20, RsaHeapInitTWL
BIOSFUNC 0x21, RsaDecryptRawTWL
BIOSFUNC 0x22, RsaDecryptUnpadTWL
BIOSFUNC 0x23, RsaDecryptDerSha1TWL

BIOSFUNC 0x24, Sha1InitTWL
BIOSFUNC 0x25, Sha1UpdateTWL
BIOSFUNC 0x26, Sha1DigestTWL
BIOSFUNC 0x27, Sha1CalcTWL
BIOSFUNC 0x28, Sha1VerifyTWL
BIOSFUNC 0x29, Sha1RandomTWL
