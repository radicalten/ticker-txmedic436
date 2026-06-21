// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <stdlib.h>

#include <calico/types.h>
#include <calico/arm/common.h>
#include <calico/arm/cache.h>
#include <calico/nds/mm.h>
#include <calico/nds/env.h>
#include <calico/nds/nitrorom.h>
#include <calico/nds/arm9/ovl.h>

static OvlParams* s_ovlTable;

bool ovlInit(void)
{
	// Succeed early if already initialized
	if (s_ovlTable) {
		return true;
	}

	// Fail early if this app has no overlays
	if (g_envAppNdsHeader->arm9_ovl_size < sizeof(OvlParams)) {
		return false;
	}

	// Open the NDS ROM
	NitroRom* nr = nitroromGetSelf();
	if (!nr) {
		return false;
	}

	// Reserve memory for the overlay table (cache line aligned)
	s_ovlTable = aligned_alloc(ARM_CACHE_LINE_SZ, g_envAppNdsHeader->arm9_ovl_size);
	if (!s_ovlTable) {
		return false;
	}

	// Read the overlay table
	if (!nitroromRead(nr, g_envAppNdsHeader->arm9_ovl_rom_offset, s_ovlTable, g_envAppNdsHeader->arm9_ovl_size)) {
		free(s_ovlTable);
		s_ovlTable = NULL;
		return false;
	}

	return true;
}

bool ovlLoadInPlace(unsigned ovl_id)
{
	// Fail early if not initialized
	if (!s_ovlTable) {
		return false;
	}

	// Succeed early if this overlay is empty
	OvlParams* ovl = &s_ovlTable[ovl_id];
	u32 total_sz = ovl->load_size + ovl->bss_size;
	if (ovl->ram_address == 0 || total_sz == 0) {
		return true;
	}

	// Load overlay file if needed
	if (ovl->load_size) {
		NitroRom* nr = nitroromGetSelf();
		if (!nr) {
			return false;
		}

		if (!nitroromReadFile(nr, ovl->file_id, 0, (void*)ovl->ram_address, ovl->load_size)) {
			return false;
		}
	}

	// Clear BSS if needed
	if (ovl->bss_size) {
		armFillMem32((void*)(ovl->ram_address + ovl->load_size), 0, ovl->bss_size);
	}

	// Flush the cache if needed
	if (ovl->ram_address >= MM_MAINRAM) {
		armDCacheFlush((void*)ovl->ram_address, total_sz);
		armICacheInvalidateAll();
	}

	return true;
}

void ovlActivate(unsigned ovl_id)
{
	// Fail early if not initialized
	if (!s_ovlTable) {
		return;
	}

	// Run static constructors
	OvlParams* ovl = &s_ovlTable[ovl_id];
	OvlStaticFn* pfn = (OvlStaticFn*)ovl->ctors_start;
	while (pfn != (OvlStaticFn*)ovl->ctors_end) {
		(*pfn)();
		pfn ++;
	}
}

void ovlDeactivate(unsigned ovl_id)
{
	// Fail early if not initialized
	if (!s_ovlTable) {
		return;
	}

	// Run static destructors (in reverse order)
	OvlParams* ovl = &s_ovlTable[ovl_id];
	OvlStaticFn* pfn = (OvlStaticFn*)(ovl->reserved &~ 3); // remove reserved flag bits
	while (pfn != (OvlStaticFn*)ovl->ctors_end) {
		pfn --;
		(*pfn)();
	}
}
