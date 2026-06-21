// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <string.h>
#include <calico/types.h>
#if defined(ARM9)
#include <calico/arm/cache.h>
#endif
#include <calico/system/irq.h>
#include <calico/system/thread.h>
#include <calico/system/mutex.h>
#include <calico/nds/system.h>
#include <calico/nds/dma.h>
#include <calico/nds/scfg.h>
#include <calico/nds/ntrcard.h>
#include "transfer.h"

#define NTRCARD_PARAM_MASK (NTRCARD_ROMCNT_GAP1_LEN(0x1fff)|NTRCARD_ROMCNT_GAP2_LEN(0x3f)|NTRCARD_ROMCNT_CLK_DIV_8)

#if defined(ARM9)
#define CACHE_ALIGN ARM_CACHE_LINE_SZ
#elif defined(ARM7)
#define CACHE_ALIGN 4
#endif

static struct {
	Mutex mutex;
	u32 romcnt;
	union {
		u32 cache_pos;
		NtrCardSecure* secure;
	};

	bool first_init;
	NtrCardMode mode;
} s_ntrcardState;

alignas(CACHE_ALIGN) u8 s_ntrcardCache[NTRCARD_SECTOR_SZ];

MK_INLINE bool _ntrcardIsOpenByArm9(void)
{
	return (REG_EXMEMCNT & EXMEMCNT_NDS_SLOT_ARM7) == 0;
}

MK_INLINE bool _ntrcardIsOpenByArm7(void)
{
	return (s_transferRegion->exmemcnt_mirror & EXMEMCNT_NDS_SLOT_ARM7) != 0;
}

MK_INLINE bool _ntrcardIsPresent(void)
{
	// If SCFG is available, check if a card is inserted and the hardware is powered on
	// If SCFG is not available, we ignore this check and succeed anyway
	return !scfgIsPresent() || (REG_SCFG_MC & (SCFG_MC_POWER_MASK|SCFG_MC_IS_EJECTED)) == SCFG_MC_POWER_ON;
}

MK_INLINE bool _ntrcardIsNoneMode(void)
{
	return s_ntrcardState.mode == NtrCardMode_None;
}

MK_INLINE bool _ntrcardIsInitMode(void)
{
	return s_ntrcardState.mode == NtrCardMode_Init;
}

MK_INLINE bool _ntrcardIsInitOrMainMode(void)
{
	return s_ntrcardState.mode == NtrCardMode_Init || s_ntrcardState.mode == NtrCardMode_Main;
}

MK_INLINE bool _ntrcardIsSecureMode(void)
{
	return s_ntrcardState.mode == NtrCardMode_Secure;
}

MK_INLINE NtrCardMode _ntrcardInitOrMainCmdImpl(NtrCardMode init_cmd, NtrCardMode main_cmd)
{
	return s_ntrcardState.mode == NtrCardMode_Main ? main_cmd : init_cmd;
}

#define _ntrcardInitOrMainCmd(_cmd) _ntrcardInitOrMainCmdImpl(NtrCardCmd_Init##_cmd, NtrCardCmd_Main##_cmd)

#if defined(ARM9)
#define _ntrcardIsOpenBySelf  _ntrcardIsOpenByArm9
#define _ntrcardIsOpenByOther _ntrcardIsOpenByArm7
#elif defined(ARM7)
#define _ntrcardIsOpenBySelf  _ntrcardIsOpenByArm7
#define _ntrcardIsOpenByOther _ntrcardIsOpenByArm9
#endif

static bool _ntrcardOpen(void)
{
	if (_ntrcardIsOpenBySelf()) {
		return true;
	}

	if (_ntrcardIsOpenByOther()) {
		return false;
	}

#if defined(ARM9)
	REG_EXMEMCNT &= ~EXMEMCNT_NDS_SLOT_ARM7;
#elif defined(ARM7)
	s_transferRegion->exmemcnt_mirror |= EXMEMCNT_NDS_SLOT_ARM7;
#endif

	irqEnable(IRQ_SLOT1_TX);

	s_ntrcardState.cache_pos = UINT32_MAX;

	if (!s_ntrcardState.first_init) {
		s_ntrcardState.first_init = true;

		if (g_envBootParam->boot_src == EnvBootSrc_Card) {
			// If we are booted from Slot-1, assume the card is in Main mode
			// (this is true for both flashcards and emulators)
			s_ntrcardState.mode = NtrCardMode_Main;
			s_ntrcardState.romcnt = g_envAppNdsHeader->cardcnt_normal;
			s_ntrcardState.romcnt &= ~NTRCARD_ROMCNT_BLK_SIZE(7);
			s_ntrcardState.romcnt |= NTRCARD_ROMCNT_START | NTRCARD_ROMCNT_NO_RESET;
		} else {
			// Otherwise, make no assumptions - leave card mode undefined
			s_ntrcardState.mode = NtrCardMode_None;
			s_ntrcardState.romcnt = 0;
		}
	}

	return true;
}

static void _ntrcardClose(void)
{
	if (!_ntrcardIsOpenBySelf()) {
		return;
	}

	irqDisable(IRQ_SLOT1_TX);
	REG_NTRCARD_CNT = 0;

#if defined(ARM9)
	REG_EXMEMCNT |= EXMEMCNT_NDS_SLOT_ARM7;
#elif defined(ARM7)
	s_transferRegion->exmemcnt_mirror &= ~EXMEMCNT_NDS_SLOT_ARM7;
#endif
}

static void _ntrcardIrqWaitIdle(void)
{
	while (REG_NTRCARD_ROMCNT & NTRCARD_ROMCNT_BUSY) {
		threadIrqWait(false, IRQ_SLOT1_TX);
	}
}

static void _ntrcardRecvByDma(u32 romcnt, void* buf, unsigned dma_ch, u32 size)
{
	dmaBusyWait(dma_ch);
#ifdef ARM9
	armDCacheInvalidate(buf, size);
#endif

	REG_DMAxSAD(dma_ch) = (uptr)&REG_NTRCARD_FIFO;
	REG_DMAxDAD(dma_ch) = (uptr)buf;

	size_t wordCount = 1;
	unsigned flags =
		DMA_MODE_DST(DmaMode_Increment) |
		DMA_MODE_SRC(DmaMode_Fixed) |
		DMA_MODE_REPEAT |
		DMA_UNIT_32 |
		DMA_TIMING(DmaTiming_Slot1) |
		DMA_START;
	_dmaSetDmaCnt(dma_ch, wordCount, flags);

	REG_NTRCARD_ROMCNT = romcnt;
	_ntrcardIrqWaitIdle();
	REG_DMAxCNT_H(dma_ch) = 0;
}

static void _ntrcardRecvByCpu(u32 romcnt, void* buf)
{
	REG_NTRCARD_ROMCNT = romcnt;

	u32* buf32 = (u32*)buf;
	do {
		romcnt = REG_NTRCARD_ROMCNT;
		if (romcnt & NTRCARD_ROMCNT_DATA_READY) {
			*buf32++ = REG_NTRCARD_FIFO;
		}
	} while (romcnt & NTRCARD_ROMCNT_BUSY);
}

static void _ntrcardDummyRecvByDma(u32 romcnt, unsigned dma_ch)
{
	dmaBusyWait(dma_ch);

	REG_DMAxSAD(dma_ch) = (uptr)&REG_NTRCARD_FIFO;
	REG_DMAxDAD(dma_ch) = (uptr)&REG_DMAxFIL(dma_ch);

	size_t wordCount = 1;
	unsigned flags =
		DMA_MODE_DST(DmaMode_Fixed) |
		DMA_MODE_SRC(DmaMode_Fixed) |
		DMA_MODE_REPEAT |
		DMA_UNIT_32 |
		DMA_TIMING(DmaTiming_Slot1) |
		DMA_START;
	_dmaSetDmaCnt(dma_ch, wordCount, flags);

	REG_NTRCARD_ROMCNT = romcnt;
	_ntrcardIrqWaitIdle();
	REG_DMAxCNT_H(dma_ch) = 0;
}

static void _ntrcardDummyRecvByCpu(u32 romcnt)
{
	REG_NTRCARD_ROMCNT = romcnt;

	do {
		romcnt = REG_NTRCARD_ROMCNT;
		if (romcnt & NTRCARD_ROMCNT_DATA_READY) {
			MK_DUMMY(REG_NTRCARD_FIFO);
		}
	} while (romcnt & NTRCARD_ROMCNT_BUSY);
}

MK_NOINLINE static void _ntrcardEnterInit(int dma_ch)
{
	while (REG_NTRCARD_ROMCNT & NTRCARD_ROMCNT_BUSY);
	REG_NTRCARD_CNT = NTRCARD_CNT_MODE_ROM | NTRCARD_CNT_TX_IE | NTRCARD_CNT_ENABLE;
	REG_NTRCARD_ROMCMD_HI = __builtin_bswap32(NtrCardCmd_Init << 24);
	REG_NTRCARD_ROMCMD_LO = 0;

	u32 romcnt =
		NTRCARD_ROMCNT_START | NTRCARD_ROMCNT_NO_RESET | NTRCARD_ROMCNT_CLK_DIV_8 |
		NTRCARD_ROMCNT_BLK_SIZE(NtrCardBlkSize_0x2000) |
		NTRCARD_ROMCNT_GAP2_LEN(0x18);

	if (dma_ch >= 0) {
		_ntrcardDummyRecvByDma(romcnt, dma_ch & 3);
	} else {
		_ntrcardDummyRecvByCpu(romcnt);
	}
}

MK_NOINLINE static void _ntrcardGetChipId(NtrChipId* out)
{
	while (REG_NTRCARD_ROMCNT & NTRCARD_ROMCNT_BUSY);
	REG_NTRCARD_CNT = NTRCARD_CNT_MODE_ROM | NTRCARD_CNT_TX_IE | NTRCARD_CNT_ENABLE;
	REG_NTRCARD_ROMCMD_HI = __builtin_bswap32(_ntrcardInitOrMainCmd(GetChipId) << 24);
	REG_NTRCARD_ROMCMD_LO = 0;

	u32 romcnt = s_ntrcardState.romcnt | NTRCARD_ROMCNT_BLK_SIZE(NtrCardBlkSize_4);
	_ntrcardRecvByCpu(romcnt, out);
}

MK_NOINLINE static void _ntrcardRomReadSector(int dma_ch, u32 offset, void* buf)
{
	while (REG_NTRCARD_ROMCNT & NTRCARD_ROMCNT_BUSY);
	REG_NTRCARD_CNT = NTRCARD_CNT_MODE_ROM | NTRCARD_CNT_TX_IE | NTRCARD_CNT_ENABLE;
	REG_NTRCARD_ROMCMD_HI = __builtin_bswap32((offset >> 8) | (_ntrcardInitOrMainCmd(RomRead) << 24));
	REG_NTRCARD_ROMCMD_LO = __builtin_bswap32(offset << 24);

	u32 romcnt = s_ntrcardState.romcnt | NTRCARD_ROMCNT_BLK_SIZE(NtrCardBlkSize_Sector);

	if (dma_ch >= 0) {
		_ntrcardRecvByDma(romcnt, buf, dma_ch & 3, NTRCARD_SECTOR_SZ);
	} else {
		_ntrcardRecvByCpu(romcnt, buf);
	}
}

bool ntrcardOpen(void)
{
	mutexLock(&s_ntrcardState.mutex);
	bool rc = _ntrcardOpen();
	mutexUnlock(&s_ntrcardState.mutex);
	return rc;
}

void ntrcardClose(void)
{
	mutexLock(&s_ntrcardState.mutex);
	_ntrcardClose();
	mutexUnlock(&s_ntrcardState.mutex);
}

NtrCardMode ntrcardGetMode(void)
{
	return s_ntrcardState.mode;
}

MK_INLINE bool _ntrcardIsAligned(int dma_ch, uptr addr)
{
	unsigned align = 4;
#ifdef ARM9
	if (dma_ch >= 0) {
		align = ARM_CACHE_LINE_SZ;
	}
#endif
	return (addr & (align-1)) == 0;
}

MK_INLINE unsigned _ntrcardCanAccess(int dma_ch, uptr addr)
{
	bool ret = _ntrcardIsAligned(dma_ch, addr);
#ifdef ARM9
	if (ret && dma_ch >= 0) {
		ret = !(addr < MM_MAINRAM || (addr >= MM_DTCM && addr < MM_TWLWRAM_MAP));
	}
#endif
	return ret;
}

void ntrcardClearState(void)
{
	mutexLock(&s_ntrcardState.mutex);
	s_ntrcardState.mode = NtrCardMode_None;
	s_ntrcardState.romcnt = 0;
	s_ntrcardState.cache_pos = UINT32_MAX;
	mutexUnlock(&s_ntrcardState.mutex);
}

void ntrcardSetParams(u32 params)
{
	mutexLock(&s_ntrcardState.mutex);
	if (!_ntrcardIsNoneMode()) {
		s_ntrcardState.romcnt &= ~NTRCARD_PARAM_MASK;
		s_ntrcardState.romcnt |= params & NTRCARD_PARAM_MASK;
	}
	mutexUnlock(&s_ntrcardState.mutex);
}

bool ntrcardStartup(int dma_ch)
{
	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsNoneMode() || !_ntrcardIsPresent()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	// Initial configuration, using conservative timings
	s_ntrcardState.mode = NtrCardMode_Init;
	s_ntrcardState.romcnt = NTRCARD_ROMCNT_START | NTRCARD_ROMCNT_NO_RESET | NTRCARD_PARAM_MASK;

	// Release RESET on the card
	REG_NTRCARD_ROMCNT = s_ntrcardState.romcnt &~ NTRCARD_ROMCNT_START;
	threadSleep(120000); // 120ms

	// Send the initialization command
	_ntrcardEnterInit(dma_ch);

	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}

bool ntrcardGetChipId(NtrChipId* out)
{
	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsInitOrMainMode()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	_ntrcardGetChipId(out);

	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}

bool ntrcardRomReadSector(int dma_ch, u32 offset, void* buf)
{
	if_unlikely (!_ntrcardCanAccess(dma_ch, (uptr)buf)) {
		return false;
	}

	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsInitOrMainMode()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	_ntrcardRomReadSector(dma_ch, offset, buf);

	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}

bool ntrcardRomRead(int dma_ch, u32 offset, void* buf, u32 size)
{
	u8* out = (u8*)buf;

	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsInitOrMainMode()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	while (size) {
		const void* copy_src = NULL;
		void* cur_buf = s_ntrcardCache;
		u32 cur_size = NTRCARD_SECTOR_SZ;

		u32 cur_sector = offset &~ (NTRCARD_SECTOR_SZ-1);
		u32 sec_offset = offset & (NTRCARD_SECTOR_SZ-1);

		if (sec_offset != 0) {
			cur_size -= sec_offset;
			cur_size = cur_size < size ? cur_size : size;
			copy_src = &s_ntrcardCache[sec_offset];
		} else if (size < cur_size) {
			cur_size = size;
			copy_src = s_ntrcardCache;
		} else if (cur_sector == s_ntrcardState.cache_pos || !_ntrcardCanAccess(dma_ch, (uptr)out)) {
			copy_src = s_ntrcardCache;
		} else {
			cur_buf = out;
		}

		if (cur_buf != s_ntrcardCache || s_ntrcardState.cache_pos != cur_sector) {
			_ntrcardRomReadSector(dma_ch, cur_sector, cur_buf);
		}

		if (copy_src) {
			s_ntrcardState.cache_pos = cur_sector;
			if_likely ((((uptr)copy_src | (uptr)out | cur_size) & 3) == 0) {
				armCopyMem32(out, copy_src, cur_size);
			} else {
				memcpy(out, copy_src, cur_size);
			}
		}

		offset += cur_size;
		out += cur_size;
		size -= cur_size;
	}

	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}

bool ntrcardEnterSecure(NtrCardSecure* secure)
{
	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsInitMode()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	unsigned cmd = secure->is_twl ? NtrCardCmd_InitEnterSecureTwl : NtrCardCmd_InitEnterSecureNtr;

	while (REG_NTRCARD_ROMCNT & NTRCARD_ROMCNT_BUSY);
	REG_NTRCARD_CNT = NTRCARD_CNT_MODE_ROM | NTRCARD_CNT_TX_IE | NTRCARD_CNT_ENABLE;
	REG_NTRCARD_ROMCMD_HI = __builtin_bswap32((cmd << 24) | (secure->fixed_arg & 0xffffff));
	REG_NTRCARD_ROMCMD_LO = __builtin_bswap32((secure->counter & 0xfffff) << 8);

	REG_NTRCARD_ROMCNT = s_ntrcardState.romcnt;
	_ntrcardIrqWaitIdle();

	s_ntrcardState.mode = NtrCardMode_Secure;
	s_ntrcardState.secure = secure;
	secure->romcnt &= NTRCARD_PARAM_MASK;
	secure->romcnt |= NTRCARD_ROMCNT_START | NTRCARD_ROMCNT_NO_RESET | (secure->is_1trom ? 0 : NTRCARD_ROMCNT_CLK_GAP_ON);

	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}

bool ntrcardSecureCmd(int dma_ch, NtrCardSecureCmd const* cmd, NtrCardBlkSize sz, void* buf)
{
	if_unlikely (sz != NtrCardBlkSize_0 && !_ntrcardCanAccess(dma_ch, (uptr)buf)) {
		return false;
	}

	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsSecureMode()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	while (REG_NTRCARD_ROMCNT & NTRCARD_ROMCNT_BUSY);
	REG_NTRCARD_CNT = NTRCARD_CNT_MODE_ROM | NTRCARD_CNT_TX_IE | NTRCARD_CNT_ENABLE;
	REG_NTRCARD_ROMCMD_HI = __builtin_bswap32(cmd->raw[1]);
	REG_NTRCARD_ROMCMD_LO = __builtin_bswap32(cmd->raw[0]);

	NtrCardSecure* secure = s_ntrcardState.secure;
	u32 romcnt = secure->romcnt;
	bool is_1trom = secure->is_1trom;

	// The hardware does not honor the sector gap for 0-byte and 4-byte transfers.
	// Adjust the parameters to add the sector gap length to the initial gap
	if (sz == NtrCardBlkSize_0 || sz == NtrCardBlkSize_4) {
		unsigned gap2_mask = NTRCARD_ROMCNT_GAP2_LEN(0x3f);
		unsigned gap2 = (romcnt & gap2_mask) / NTRCARD_ROMCNT_GAP2_LEN(1);
		romcnt &= ~gap2_mask;
		romcnt += NTRCARD_ROMCNT_GAP1_LEN(gap2);
	}

	// 1T-ROM cards require extra time to decode the Blowfish-encrypted command
	if (is_1trom) {
		REG_NTRCARD_ROMCNT = romcnt;
		threadSleepTicks(secure->delay * (256/64));
		_ntrcardIrqWaitIdle();
	}

	if (sz == NtrCardBlkSize_0) {
		// 0-byte transfers can be issued right away
		REG_NTRCARD_ROMCNT = romcnt;
		_ntrcardIrqWaitIdle();
	} else {
		// Calculate number and size of blocks to transfer
		unsigned blk_sz;
		unsigned num_blks;
		if (sz == NtrCardBlkSize_4) {
			// 4-byte transfers need no special handling
			blk_sz = 4;
			num_blks = 1;
		} else if (is_1trom) {
			// 1T-ROM cards need transfers broken up in 0x200-sized blocks
			blk_sz = NTRCARD_SECTOR_SZ;
			num_blks = 1U << (sz-1);
			sz = NtrCardBlkSize_0x200;
		} else {
			// MROM cards can do all transfers in one go
			blk_sz = NTRCARD_SECTOR_SZ << (sz-1);
			num_blks = 1;
		}

		// Set block size
		romcnt |= NTRCARD_ROMCNT_BLK_SIZE(sz);

		// Transfer each block
		for (unsigned i = 0; i < num_blks; i ++) {
			if (dma_ch >= 0) {
				_ntrcardRecvByDma(romcnt, buf, dma_ch, blk_sz);
			} else {
				_ntrcardRecvByCpu(romcnt, buf);
			}

			buf = (u8*)buf + blk_sz;
		}
	}

	secure->counter ++;
	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}

bool ntrcardSetPngSeed(u64 seed0, u64 seed1)
{
	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsSecureMode()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	REG_NTRCARD_SEED0_LO = seed0;
	REG_NTRCARD_SEED1_LO = seed1;
	REG_NTRCARD_SEED0_HI = (seed0>>32) & 0x7f;
	REG_NTRCARD_SEED1_HI = (seed1>>32) & 0x7f;
	REG_NTRCARD_ROMCNT = NTRCARD_ROMCNT_ENCR_DATA | NTRCARD_ROMCNT_ENCR_ENABLE | NTRCARD_ROMCNT_SEED_APPLY | NTRCARD_ROMCNT_NO_RESET;

	s_ntrcardState.secure->romcnt |= NTRCARD_ROMCNT_ENCR_DATA | NTRCARD_ROMCNT_ENCR_ENABLE;

	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}

bool ntrcardLeaveSecure(void)
{
	mutexLock(&s_ntrcardState.mutex);

	if_unlikely (!_ntrcardIsOpenBySelf() || !_ntrcardIsSecureMode()) {
		mutexUnlock(&s_ntrcardState.mutex);
		return false;
	}

	s_ntrcardState.mode = NtrCardMode_Main;
	s_ntrcardState.romcnt |= NTRCARD_ROMCNT_ENCR_DATA | NTRCARD_ROMCNT_ENCR_ENABLE | NTRCARD_ROMCNT_ENCR_CMD;
	s_ntrcardState.cache_pos = UINT32_MAX;

	mutexUnlock(&s_ntrcardState.mutex);
	return true;
}
