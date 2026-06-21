// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <string.h>
#include <calico/types.h>
#if defined(ARM9)
#include <calico/arm/cache.h>
#include <calico/arm/mpu.h>
#endif
#include <calico/system/mutex.h>
#include <calico/nds/mm.h>
#include <calico/nds/gbacart.h>
#include "transfer.h"

static struct {
	Mutex mutex;
	u8 timing;
	u8 timing_backup;
} s_gbacartState;

MK_INLINE bool _gbacartIsOpenByArm9(void)
{
	return (REG_EXMEMCNT & EXMEMCNT_GBA_SLOT_ARM7) == 0;
}

MK_INLINE bool _gbacartIsOpenByArm7(void)
{
	return (s_transferRegion->exmemcnt_mirror & EXMEMCNT_GBA_SLOT_ARM7) != 0;
}

#if defined(ARM9)
#define _gbacartIsOpenBySelf  _gbacartIsOpenByArm9
#define _gbacartIsOpenByOther _gbacartIsOpenByArm7
#elif defined(ARM7)
#define _gbacartIsOpenBySelf  _gbacartIsOpenByArm7
#define _gbacartIsOpenByOther _gbacartIsOpenByArm9
#endif

static unsigned _gbacartSetTiming(unsigned mask, unsigned timing, bool force)
{
	if (!force && !_gbacartIsOpenBySelf()) {
		return 0;
	}

	unsigned old = REG_EXMEMCNT;
	unsigned new = (old &~ mask) | (timing & mask);
	if (mask) {
		REG_EXMEMCNT = new;
	}
	return old & 0x7f;
}

static bool _gbacartOpen(void)
{
	if (_gbacartIsOpenBySelf()) {
		return true;
	}

	if (_gbacartIsOpenByOther()) {
		return false;
	}

#if defined(ARM9)
	REG_EXMEMCNT &= ~EXMEMCNT_GBA_SLOT_ARM7;
#elif defined(ARM7)
	s_transferRegion->exmemcnt_mirror |= EXMEMCNT_GBA_SLOT_ARM7;
#endif

	// Set initial identification timings
	s_gbacartState.timing_backup = _gbacartSetTiming(GBA_ALL_MASK, GBA_WAIT_ROM_N_18, true);

#if defined(ARM9)
	armMpuSetRegionDataPerm(2, CP15_PU_PERM_RW);
	armMpuSetRegionAddrSize(2, MM_CARTROM, CP15_PU_64M);
#endif

	return true;
}

static void _gbacartClose(void)
{
	if (!_gbacartIsOpenBySelf()) {
		return;
	}

	_gbacartSetTiming(GBA_ALL_MASK, s_gbacartState.timing_backup, true);

#if defined(ARM9)
	armMpuClearRegion(2);

	REG_EXMEMCNT |= EXMEMCNT_GBA_SLOT_ARM7;
#elif defined(ARM7)
	s_transferRegion->exmemcnt_mirror &= ~EXMEMCNT_GBA_SLOT_ARM7;
#endif
}

bool gbacartOpen(void)
{
	if (systemIsTwlMode()) {
		return false;
	}

	mutexLock(&s_gbacartState.mutex);
	bool rc = _gbacartOpen();
	mutexUnlock(&s_gbacartState.mutex);
	return rc;
}

void gbacartClose(void)
{
	mutexLock(&s_gbacartState.mutex);
	_gbacartClose();
	mutexUnlock(&s_gbacartState.mutex);
}

bool gbacartIsOpen(void)
{
	return _gbacartIsOpenBySelf();
}

unsigned gbacartSetTiming(unsigned mask, unsigned timing)
{
	mask &= GBA_ALL_MASK;
	mutexLock(&s_gbacartState.mutex);
	unsigned old = _gbacartSetTiming(mask, timing, false);
	mutexUnlock(&s_gbacartState.mutex);
	return old;
}
