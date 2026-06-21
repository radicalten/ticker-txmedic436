// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <calico/types.h>

#define CRT0_MAGIC_ARM7 0x37444f4d // MOD7
#define CRT0_MAGIC_ARM9 0x39444f4d // MOD9

typedef struct Crt0LoadListEntry {
	uptr start;
	uptr end;
	uptr bss_end;
} Crt0LoadListEntry;

typedef struct Crt0LoadList {
	uptr lma;
	Crt0LoadListEntry const* start;
	Crt0LoadListEntry const* end;
} Crt0LoadList;

typedef struct Crt0Header {
	u32 magic;
	u16 flags;
	u16 hdr_sz;
	Crt0LoadList ll_ntr;
	Crt0LoadList ll_twl;
} Crt0Header;

typedef struct Crt0Header9 {
	Crt0Header base;
	u32 stack_size;
	uptr dldi_addr;
} Crt0Header9;

MK_EXTERN32 void crt0CopyMem32(uptr dst, uptr src, uptr size);
MK_EXTERN32 void crt0FillMem32(uptr dst, u32 value, uptr size);

MK_INLINE bool crt0IsValidHeader(Crt0Header const* hdr, u32 magic)
{
	return hdr->magic == magic && (hdr->flags & 1) != 0;
}
