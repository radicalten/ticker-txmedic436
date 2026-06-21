// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#if defined(ARM9)
#include <calico/arm/cache.h>
#endif
#include <calico/nds/mm.h>
#include <calico/nds/system.h>
#include <calico/nds/scfg.h>
#include <calico/nds/dma.h>
#include <calico/nds/ndma.h>
#include <calico/nds/timer.h>
#include <calico/nds/irq.h>
#include <calico/nds/env.h>
#if defined(ARM7)
#include <calico/nds/arm7/gpio.h>
#endif
#include "crt0.h"

bool g_isTwlMode;
#if defined(ARM7)
bool g_cdcIsTwlMode;
#endif

extern void *fake_heap_start, *fake_heap_end;

void _threadInit(void);
void _pxiInit(void);

#if defined(ARM9)

void crt0SetupMPU(bool is_twl);
void systemStartup(void);

extern char __heap_start_ntr[], __heap_start_twl[];

// Main thread stack size (0 = default)
MK_WEAK u32 __stacksize__ = 0;

#elif defined(ARM7)

void build_argv(EnvNdsArgvHeader*);

extern char __heap_start[], __heap_end[];

#endif

MK_NOINLINE void crt0ProcessLoadList(Crt0LoadList const* ll)
{
	uptr lma = ll->lma;
	for (Crt0LoadListEntry const* ent = ll->start; ent != ll->end; ent ++) {
		uptr size = ent->end - ent->start;
		uptr bss_start = ent->end;
		uptr bss_size = ent->bss_end - bss_start;
		if (size != 0) {
			crt0CopyMem32(ent->start, lma, size);
			lma += size;
		}
		if (bss_size != 0) {
			crt0FillMem32(bss_start, 0, bss_size);
		}
	}
}

#if defined(ARM7)

static void crt0SetupArgv(bool is_twl)
{
	uptr argmem = MM_ENV_FREE_F000; // TEMP TEMP: Maybe we should move this somewhere else

	// On DSi, check for the presence of the device list
	if (is_twl) {
		EnvTwlDeviceList* devlist = (void*)g_envAppTwlHeader->device_list_ram_address;
		if (devlist && devlist->argv0[0] != 0) {
			// Present - relocate it to the shared main RAM area if needed
			if (devlist != g_envTwlDeviceList) {
				crt0CopyMem32((uptr)g_envTwlDeviceList, (uptr)devlist, sizeof(EnvTwlDeviceList));
			}
		} else {
			// Not present - clear out the shared main RAM area
			crt0FillMem32((uptr)g_envTwlDeviceList, 0, sizeof(EnvTwlDeviceList));
		}
	}

	// Check if we were supplied an argument string
	if (g_envNdsArgvHeader->magic == ENV_NDS_ARGV_MAGIC) {
		// Copy argument string to safe memory
		__builtin_memcpy((void*)argmem, g_envNdsArgvHeader->args_str, g_envNdsArgvHeader->args_str_size);
		g_envNdsArgvHeader->args_str = (char*)argmem;

		// Build argv!
		build_argv(g_envNdsArgvHeader);
	} else {
		// Initialize empty argv
		crt0FillMem32((uptr)g_envNdsArgvHeader, 0, sizeof(EnvNdsArgvHeader));
		g_envNdsArgvHeader->magic = ENV_NDS_ARGV_MAGIC;
		g_envNdsArgvHeader->argv = (char**)argmem;

		// Check if we have a boot filename from the DSi device list
		char* boot_name = NULL;
		if (is_twl && g_envTwlDeviceList->argv0[0] != 0) {
			boot_name = g_envTwlDeviceList->argv0;
		}

		// If we do: add it to argv
		if (boot_name) {
			g_envNdsArgvHeader->argc = 1;
			g_envNdsArgvHeader->argv[0] = boot_name;
		}

		// Terminate argv list
		g_envNdsArgvHeader->argv[g_envNdsArgvHeader->argc] = NULL;
	}
}

MK_WEAK void _blkShelterDldi(void)
{
}

#endif

#if defined(ARM9)
#define _EXTRA_ARGS , void* heap_end
#else
#define _EXTRA_ARGS
#endif

void crt0Startup(Crt0Header const* hdr, bool is_twl _EXTRA_ARGS)
{
#if defined(ARM9)
	// Configure the protection unit
	crt0SetupMPU(is_twl);

	// Shelter the header on stack in order to prevent LMA/VMA overlaps from overwriting it
	Crt0Header9 hdr9;
	crt0CopyMem32((uptr)&hdr9, (uptr)hdr, sizeof(hdr9));
	hdr = &hdr9.base;
#endif

	// Clear DMA and timers
	for (unsigned i = 0; i < 4; i ++) {
		REG_DMAxCNT_H(i) = 0;
		REG_TMxCNT_H(i) = 0;
	}

	if (is_twl) {
		// Initialize and clear New DMA
		REG_NDMAGCNT = NDMA_G_ROUND_ROBIN |
#if defined(ARM9)
			NDMA_G_RR_CYCLES(32);
#elif defined(ARM7)
			NDMA_G_RR_CYCLES(16);
#endif
		for (unsigned i = 0; i < 4; i ++) {
			REG_NDMAxCNT(i) = 0;
		}
	}

#if defined(ARM7)
	// Clear some memory
	crt0FillMem32(MM_ENV_FREE_D000, 0, MM_ENV_FREE_D000_SZ);
	crt0FillMem32(MM_ENV_FREE_F000, 0, MM_ENV_FREE_F000_SZ);
	crt0FillMem32(MM_ENV_FREE_FCF0, 0, MM_ENV_FREE_FCF0_SZ);
	crt0FillMem32(MM_ENV_FREE_FDA0, 0, MM_ENV_FREE_FDA0_SZ);
	crt0FillMem32(MM_ENV_FREE_FF60, 0, MM_ENV_FREE_FF60_SZ);

	// Configure GBA/NDS GPIO (GPIO mode, SI (RTC) irq enable, pull-up on SI)
	REG_RCNT = RCNT_MODE_GPIO | RCNT_SI_IRQ_ENABLE | RCNT_SI_DIR_OUT | RCNT_SI;

	if (is_twl) {
		// Ensure WRAM_A is mapped to the right location
		REG_MBK_MAP_A = g_envAppTwlHeader->arm7_mbk_map_settings[0];

		// Configure DSi GPIO
		REG_GPIO_CNT = GPIO_CNT_DIR_OUT(GPIO_PIN_SOUND_ENABLE) | GPIO_CNT_PIN(GPIO_PIN_SOUND_ENABLE);
		REG_GPIO_IRQ = GPIO_IRQ_ENABLE(GPIO_PIN_MCUIRQ);
	}
#elif defined(ARM9)
	// By default, ARM7 owns GBA/NDS slots & has Main RAM priority
	REG_EXMEMCNT |= EXMEMCNT_GBA_SLOT_ARM7 | EXMEMCNT_NDS_SLOT_ARM7 | EXMEMCNT_MAIN_RAM_PRIO_ARM7;
#endif

	// Process load lists
	crt0ProcessLoadList(&hdr->ll_ntr);
	if (is_twl) {
		crt0ProcessLoadList(&hdr->ll_twl);
	}

#if defined(ARM9)
	// Flush data cache and invalidate instruction cache
	armDCacheFlushAll();
	armICacheInvalidateAll();
#endif

	// Back up DSi mode flag
	g_isTwlMode = is_twl;
#if defined(ARM7)
	if (is_twl) {
		// Back up DSi mode codec flag too
		g_cdcIsTwlMode = g_envAppTwlHeader->twl_flags2 & 1;
	}
#endif

#if defined(ARM9)
	// Honor stack size override if specified
	if (hdr9.stack_size) {
		__stacksize__ = hdr9.stack_size;
	}
#endif

	// Set up newlib heap
#if defined(ARM9)
	fake_heap_start = is_twl ? __heap_start_twl : __heap_start_ntr;
	fake_heap_end = heap_end;
#elif defined(ARM7)
	fake_heap_start = __heap_start;
	fake_heap_end = __heap_end;
#endif

#if defined(ARM7)
	// Set up argv
	crt0SetupArgv(is_twl);
#endif

	// Initialize the interrupt controller
	REG_IE = 0;
	REG_IF = ~0;
#if defined(ARM7)
	if (is_twl) {
		REG_IE2 = 0;
		REG_IF2 = ~0;
	}
#endif
	REG_IME = 1;

	// Initialize the threading system
	_threadInit();

	// Initialize the inter-processor communication system
	_pxiInit();

#if defined(ARM9)
	// Call pre-libc system startup routine
	systemStartup();
#elif defined(ARM7)
	// Shelter DLDI driver from ARM9 executable
	_blkShelterDldi();
#endif
}
