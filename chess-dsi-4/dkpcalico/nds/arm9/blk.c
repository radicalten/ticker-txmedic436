// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/arm/cache.h>
#include <calico/system/thread.h>
#include <calico/system/mailbox.h>
#include <calico/dev/blk.h>
#include <calico/dev/dldi.h>
#include <calico/nds/mm.h>
#include <calico/nds/pxi.h>

#include "../transfer.h"
#include "../pxi/blkdev.h"

// Ensure DLDI stub is linked in
extern char __dldi_start[];
char* const __dldi_ref = __dldi_start;

static BlkDevCallbackFn s_blkDevCallback;

static Mailbox s_blkPxiMailbox;
static u32 s_blkPxiMailboxData[1];
static Thread s_blkPxiThread;
alignas(8) static u8 s_blkPxiThreadStack[2048];

MK_CONSTEXPR bool _blkIsValidAddr(const void* addr, u32 alignment)
{
	// Verify that:
	// 1) The address is in main RAM (excluding DTCM mapped/shared region)
	// 2) The alignment is correct (cacheline for reads, word for writes)
	// Note that we only verify the start address and not the entire range.
	// This is on purpose, as the only reason why this check is here is catching
	// stupid usage bugs, not preventing "exploits".

	uptr p = (uptr)addr;
	return (p & (alignment-1)) == 0 && p >= MM_MAINRAM && p < MM_DTCM;
}

static int _blkPxiThread(void* unused)
{
	for (;;) {
		u32 msg = mailboxRecv(&s_blkPxiMailbox);

		PxiBlkDevMsgType type = pxiBlkDevMsgGetType(msg);
		u32 imm = pxiBlkDevMsgGetImmediate(msg);

		switch (type) {
			default: break;

			case PxiBlkDevMsg_Removed:
			case PxiBlkDevMsg_Inserted:
				if (s_blkDevCallback) {
					s_blkDevCallback((BlkDevice)imm, type == PxiBlkDevMsg_Inserted);
				}
				break;
		}
	}

	return 0;
}

void blkInit(void)
{
	pxiWaitRemote(PxiChannel_BlkDev);
}

void blkSetDevCallback(BlkDevCallbackFn fn)
{
	// Bring up PXI event mailbox/thread if necessary
	if (fn && !threadIsValid(&s_blkPxiThread)) {
		mailboxPrepare(&s_blkPxiMailbox, s_blkPxiMailboxData, sizeof(s_blkPxiMailboxData)/sizeof(u32));
		pxiSetMailbox(PxiChannel_BlkDev, &s_blkPxiMailbox);
		threadPrepare(&s_blkPxiThread, _blkPxiThread, NULL, &s_blkPxiThreadStack[sizeof(s_blkPxiThreadStack)], 0x08);
		threadAttachLocalStorage(&s_blkPxiThread, NULL);
		threadStart(&s_blkPxiThread);
	}

	s_blkDevCallback = fn;
}

bool blkDevIsPresent(BlkDevice dev)
{
	return pxiSendAndReceive(PxiChannel_BlkDev, pxiBlkDevMakeMsg(PxiBlkDevMsg_IsPresent, dev));
}

bool blkDevInit(BlkDevice dev)
{
	return pxiSendAndReceive(PxiChannel_BlkDev, pxiBlkDevMakeMsg(PxiBlkDevMsg_Init, dev));
}

u32 blkDevGetSectorCount(BlkDevice dev)
{
	if (dev == BlkDevice_TwlNandAes) {
		dev = BlkDevice_TwlNand;
	}

	if (dev >= BlkDevice_Dldi && dev <= BlkDevice_TwlNand) {
		return s_transferRegion->blkdev_sector_count[dev];
	} else {
		return 0;
	}
}

MK_NOINLINE static bool _blkDevReadWriteSectors(u32 msg, u32 buffer, u32 first_sector, u32 num_sectors)
{
	u32 params[3] = {
		buffer,
		first_sector,
		num_sectors,
	};

	return pxiSendWithDataAndReceive(PxiChannel_BlkDev, msg, params, sizeof(params)/sizeof(u32));
}

bool blkDevReadSectors(BlkDevice dev, void* buffer, u32 first_sector, u32 num_sectors)
{
	if (!_blkIsValidAddr(buffer, ARM_CACHE_LINE_SZ)) {
		return false;
	}

	armDCacheInvalidate(buffer, num_sectors*BLK_SECTOR_SZ);
	return _blkDevReadWriteSectors(
		pxiBlkDevMakeMsg(PxiBlkDevMsg_ReadSectors, dev),
		(u32)buffer, first_sector, num_sectors);
}

bool blkDevWriteSectors(BlkDevice dev, const void* buffer, u32 first_sector, u32 num_sectors)
{
	if (!_blkIsValidAddr(buffer, 4)) {
		return false;
	}

	armDCacheFlush((void*)buffer, num_sectors*BLK_SECTOR_SZ);
	return _blkDevReadWriteSectors(
		pxiBlkDevMakeMsg(PxiBlkDevMsg_WriteSectors, dev),
		(u32)buffer, first_sector, num_sectors);
}

bool dldiDumpInternal(void* buffer)
{
	if (!_blkIsValidAddr(buffer, ARM_CACHE_LINE_SZ)) {
		return false;
	}

	u32 params[1] = {
		(u32)buffer,
	};

	armDCacheInvalidate(buffer, DLDI_MAX_ALLOC_SZ);
	return pxiSendWithDataAndReceive(PxiChannel_BlkDev,
		pxiBlkDevMakeMsg(PxiBlkDevMsg_DumpDldi, 0), params, sizeof(params)/sizeof(u32));
}
