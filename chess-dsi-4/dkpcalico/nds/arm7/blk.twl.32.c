// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/system/mutex.h>
#include <calico/dev/tmio.h>
#include <calico/dev/sdmmc.h>
#include <calico/dev/blk.h>
#include <calico/nds/bios.h>
#include <calico/nds/ndma.h>
#include <calico/nds/arm7/aes.h>
#include <calico/nds/arm7/twlblk.h>

#include "../transfer.h"

//#define TWLBLK_DEBUG

#ifdef TWLBLK_DEBUG
#include <calico/system/dietprint.h>
#else
#define dietPrint(...) ((void)0)
#endif

static TmioCtl s_sdmcCtl;
static u32 s_sdmcCtlBuf[4];
static Thread s_sdmcThread;
alignas(8) static u8 s_sdmcThreadStack[1024];

static SdmmcCard s_sdmcDevSd, s_sdmcDevNand;
static AesBlock s_sdmcNandAesIv;

static void _sdmcIrqHandler(void)
{
	tmioIrqHandler(&s_sdmcCtl);
}

bool twlblkInit(void)
{
	if (!tmioInit(&s_sdmcCtl, MM_IO + IO_TMIO0_BASE, MM_IO + IO_TMIO0_FIFO, s_sdmcCtlBuf, sizeof(s_sdmcCtlBuf)/sizeof(u32))) {
		dietPrint("[TWLBLK] TMIO init failed\n");
		return false;
	}

	threadPrepare(&s_sdmcThread, (ThreadFunc)tmioThreadMain, &s_sdmcCtl, &s_sdmcThreadStack[sizeof(s_sdmcThreadStack)], 0x10);
	threadStart(&s_sdmcThread);

	irqSet2(IRQ2_TMIO0, _sdmcIrqHandler);
	irqEnable2(IRQ2_TMIO0);

	return true;
}

bool twlSdInit(void)
{
	if (s_transferRegion->blkdev_sector_count[BlkDevice_TwlSdCard]) {
		// Already initialized
		return true;
	}

	bool ret = sdmmcCardInit(&s_sdmcDevSd, &s_sdmcCtl, 0, false);
	if (ret) {
		s_transferRegion->blkdev_sector_count[BlkDevice_TwlSdCard] = s_sdmcDevSd.num_sectors;
	}

	return ret;
}

bool twlNandInit(void)
{
	if (s_transferRegion->blkdev_sector_count[BlkDevice_TwlNand]) {
		// Already initialized
		return true;
	}

	// Try importing NAND driver state from the struct in shared main RAM
	bool ret = sdmmcCardInitFromState(&s_sdmcDevNand, &s_sdmcCtl, (void*)MM_ENV_TWL_NAND_INFO);
	if (!ret) {
		// If that fails, try initializing NAND from scratch
		ret = sdmmcCardInit(&s_sdmcDevNand, &s_sdmcCtl, 1, true);
		if (ret) {
			// Repopulate the state struct in shared main RAM
			sdmmcCardDumpState(&s_sdmcDevNand, (void*)MM_ENV_TWL_NAND_INFO);
		}
	}

	if (ret) {
		s_transferRegion->blkdev_sector_count[BlkDevice_TwlNand] = s_sdmcDevNand.num_sectors;
	} else {
		dietPrint("[TWLBLK] NAND init failed\n");
		return false;
	}

	// Calculate AES-CTR IV for NAND
	union {
		u8 sha1_digest[SVC_SHA1_DIGEST_SZ];
		AesBlock iv;
	} u;
	svcSha1CalcTWL(u.sha1_digest, &s_sdmcDevNand.cid, sizeof(s_sdmcDevNand.cid));
	s_sdmcNandAesIv = u.iv;
	dietPrint("NAND IV: %.8lX %.8lX\n         %.8lX %.8lX\n",
		s_sdmcNandAesIv.data[0], s_sdmcNandAesIv.data[1], s_sdmcNandAesIv.data[2], s_sdmcNandAesIv.data[3]);

	// Ensure NAND AES keyslot configuration is complete
	aesBusyWaitReady();
	REG_AES_SLOTxY(AesKeySlot_Nand).data[3] = 0xe1a00005;

	return true;
}

bool twlSdIsInserted(void)
{
	return true; // TODO
}

MK_INLINE void _twlblkSetupDma(unsigned ch,
	uptr src, NdmaMode srcmode, uptr dst, NdmaMode dstmode,
	u32 unit_words, u32 total_words, u32 cnt)
{
	REG_NDMAxSAD(ch) = src;
	REG_NDMAxDAD(ch) = dst;
	REG_NDMAxBCNT(ch) = 0;
	REG_NDMAxTCNT(ch) = total_words;
	REG_NDMAxWCNT(ch) = unit_words;
	REG_NDMAxCNT(ch) =
		NDMA_DST_MODE(dstmode) |
		NDMA_SRC_MODE(srcmode) |
		NDMA_BLK_WORDS(unit_words) |
		cnt;
}

static void _twlblkDmaEnd(TmioTx* tx)
{
	if (tx->status == 0) {
		// Success - wait for the DMA to be completed
		dietPrint("[TWLBLK] DMA End (OK)\n");
		ndmaBusyWait(3);
	} else {
		// Error - cancel the DMA
		dietPrint("[TWLBLK] DMA End (Error)\n");
		REG_NDMAxCNT(3) = 0;
	}
}

static void _twlblkDmaRead(TmioCtl* ctl, TmioTx* tx)
{
	if (tx->status & TMIO_STAT_CMD_BUSY) {
		dietPrint("[TWLBLK] DMA Start (Read)\n");
		_twlblkSetupDma(3,
			ctl->fifo_base, NdmaMode_Fixed, (uptr)tx->user, NdmaMode_Increment,
			BLK_SECTOR_SZ_WORDS, tx->num_blocks*BLK_SECTOR_SZ_WORDS,
			NDMA_TIMING(NdmaTiming_Tmio0) | NDMA_TX_MODE(NdmaTxMode_Timing) | NDMA_START);
	} else {
		_twlblkDmaEnd(tx);
	}
}

static void _twlblkDmaWrite(TmioCtl* ctl, TmioTx* tx)
{
	if (tx->status & TMIO_STAT_CMD_BUSY) {
		dietPrint("[TWLBLK] DMA Start (Write)\n");
		_twlblkSetupDma(3,
			(uptr)tx->user, NdmaMode_Increment, ctl->fifo_base, NdmaMode_Fixed,
			BLK_SECTOR_SZ_WORDS, tx->num_blocks*BLK_SECTOR_SZ_WORDS,
			NDMA_TIMING(NdmaTiming_Tmio0) | NDMA_TX_MODE(NdmaTxMode_Timing) | NDMA_START);
	} else {
		_twlblkDmaEnd(tx);
	}
}

// Note: In theory there is a maximum of 0xffff sectors per read due to TMIO's
// counters being 16-bit. In practice this amounts to ~32 MiB, a size which
// is double the size of main RAM*. So we basically do not bother breaking up
// the read/write into smaller chunks (this is necessary though for AES-DMA).
//
// * Debug DSi does have 32 MiB main RAM, but it is still extremely dubious to
// want to overwrite the entire RAM with data loaded in one go from SD/NAND.

bool twlSdReadSectors(void* buffer, u32 first_sector, u32 num_sectors)
{
	TmioTx tx;
	tx.callback = _twlblkDmaRead;
	tx.xfer_isr = NULL;
	tx.user = buffer;

	return sdmmcCardReadSectors(&s_sdmcDevSd, &tx, first_sector, num_sectors);
}

bool twlSdWriteSectors(const void* buffer, u32 first_sector, u32 num_sectors)
{
	TmioTx tx;
	tx.callback = _twlblkDmaWrite;
	tx.xfer_isr = NULL;
	tx.user = (void*)buffer;

	return sdmmcCardWriteSectors(&s_sdmcDevSd, &tx, first_sector, num_sectors);
}

bool twlNandReadSectors(void* buffer, u32 first_sector, u32 num_sectors)
{
	TmioTx tx;
	tx.callback = _twlblkDmaRead;
	tx.xfer_isr = NULL;
	tx.user = buffer;

	return sdmmcCardReadSectors(&s_sdmcDevNand, &tx, first_sector, num_sectors);
}

bool twlNandWriteSectors(const void* buffer, u32 first_sector, u32 num_sectors)
{
	TmioTx tx;
	tx.callback = _twlblkDmaWrite;
	tx.xfer_isr = NULL;
	tx.user = (void*)buffer;

	return sdmmcCardWriteSectors(&s_sdmcDevNand, &tx, first_sector, num_sectors);
}

static void _twlblkAesPrepare(TmioTx* tx)
{
	aesBusyWaitReady();
	aesSelectKeySlot(AesKeySlot_Nand);

	AesBlock iv = s_sdmcNandAesIv;
	aesCtrIncrementIv(&iv, tx->arg / AES_BLOCK_SZ); // Assuming byte-address for eMMC
	REG_AES_IV = iv;

	REG_AES_LEN = (tx->num_blocks*(BLK_SECTOR_SZ/AES_BLOCK_SZ)) << 16;
}

static void _twlblkAesStart(void)
{
	REG_AES_CNT =
		AES_WRFIFO_FLUSH | AES_RDFIFO_FLUSH |
		AES_WRFIFO_DMA_SIZE(AesWrfifoDma_16) | AES_RDFIFO_DMA_SIZE(AesRdfifoDma_16) |
		AES_MODE(AesMode_Ctr) |
		AES_ENABLE;
}

static void _twlblkAesDmaFinish(TmioTx* tx)
{
	if (tx->status == 0) {
		// Success - wait for the DMA to be completed
		dietPrint("[TWLBLK] AES-DMA End (OK)\n");
		ndmaBusyWait(3);
	} else {
		// Error - cancel the DMA
		dietPrint("[TWLBLK] AES-DMA End (Error)\n");
		REG_NDMAxCNT(2) = 0;
		REG_NDMAxCNT(3) = 0;
		REG_AES_CNT = AES_WRFIFO_FLUSH | AES_RDFIFO_FLUSH;
	}
}

static void _twlblkAesDmaRead(TmioCtl* ctl, TmioTx* tx)
{
	if (tx->status & TMIO_STAT_CMD_BUSY) {
		dietPrint("[TWLBLK] AES-DMA Start (Read)\n");
		_twlblkAesPrepare(tx);

		_twlblkSetupDma(2,
			ctl->fifo_base, NdmaMode_Fixed, (uptr)&REG_AES_WRFIFO, NdmaMode_Fixed,
			AES_FIFO_SZ_WORDS, AES_FIFO_SZ_WORDS,
			NDMA_TX_MODE(NdmaTxMode_Immediate));

		_twlblkSetupDma(3,
			(uptr)&REG_AES_RDFIFO, NdmaMode_Fixed, (uptr)tx->user, NdmaMode_Increment,
			AES_FIFO_SZ_WORDS, tx->num_blocks*BLK_SECTOR_SZ_WORDS,
			NDMA_TIMING(NdmaTiming_AesRdFifo) | NDMA_TX_MODE(NdmaTxMode_Timing) | NDMA_START);

		_twlblkAesStart();
	} else {
		_twlblkAesDmaFinish(tx);
	}
}

static void _twlblkAesDmaWrite(TmioCtl* ctl, TmioTx* tx)
{
	if (tx->status & TMIO_STAT_CMD_BUSY) {
		dietPrint("[TWLBLK] AES-DMA Start (Write)\n");
		_twlblkAesPrepare(tx);

		_twlblkSetupDma(2,
			(uptr)&REG_AES_RDFIFO, NdmaMode_Fixed, ctl->fifo_base, NdmaMode_Fixed,
			AES_FIFO_SZ_WORDS, AES_FIFO_SZ_WORDS,
			NDMA_TX_MODE(NdmaTxMode_Immediate));

		_twlblkSetupDma(3,
			(uptr)tx->user, NdmaMode_Increment, (uptr)&REG_AES_WRFIFO, NdmaMode_Fixed,
			AES_FIFO_SZ_WORDS, tx->num_blocks*BLK_SECTOR_SZ_WORDS,
			NDMA_TIMING(NdmaTiming_AesWrFifo) | NDMA_TX_MODE(NdmaTxMode_Timing) | NDMA_START);

		_twlblkAesStart();
	} else {
		_twlblkAesDmaFinish(tx);
	}
}

// Sadness: NDMA on DSi cannot do device-to-device timing, unlike on 3DS.
// As a workaround, it is necessary to implement the source-timing on the CPU.
// Note that the ARM7 has no cache, and as such the instruction fetch stage
// ends up stalled anyway (similar to a lengthy ISR) during DMA transfers.

static void _twlblkAesDmaXferRecv(TmioCtl* ctl, TmioTx* tx)
{
	for (size_t i = 0; i < BLK_SECTOR_SZ; i += AES_FIFO_SZ) {
		aesBusyWaitWrFifoReady();
		ndmaBusyWait(2); // mostly only for show - see above comment
		REG_NDMAxCNT(2) |= NDMA_START;
	}
}

static void _twlblkAesDmaXferSend(TmioCtl* ctl, TmioTx* tx)
{
	for (size_t i = 0; i < BLK_SECTOR_SZ; i += AES_FIFO_SZ) {
		aesBusyWaitRdFifoReady();
		REG_NDMAxCNT(2) |= NDMA_START;
		ndmaBusyWait(2); // mostly only for show - see above comment
	}
}

bool twlNandReadSectorsAes(void* buffer, u32 first_sector, u32 num_sectors)
{
	TmioTx tx;
	tx.callback = _twlblkAesDmaRead;
	tx.xfer_isr = _twlblkAesDmaXferRecv;
	tx.user = buffer;

	bool ret = false;
	while (num_sectors) {
		const u32 max_sectors = AES_MAX_PAYLOAD_SZ/BLK_SECTOR_SZ;
		u32 cur_sectors = num_sectors > max_sectors ? max_sectors : num_sectors;

		ret = sdmmcCardReadSectors(&s_sdmcDevNand, &tx, first_sector, cur_sectors);
		if (!ret) {
			break;
		}

		tx.user = (u8*)tx.user + cur_sectors*BLK_SECTOR_SZ;
		first_sector += cur_sectors;
		num_sectors -= cur_sectors;
	}

	return ret;
}

bool twlNandWriteSectorsAes(const void* buffer, u32 first_sector, u32 num_sectors)
{
	TmioTx tx;
	tx.callback = _twlblkAesDmaWrite;
	tx.xfer_isr = _twlblkAesDmaXferSend;
	tx.user = (void*)buffer;

	bool ret = false;
	while (num_sectors) {
		const u32 max_sectors = AES_MAX_PAYLOAD_SZ/BLK_SECTOR_SZ;
		u32 cur_sectors = num_sectors > max_sectors ? max_sectors : num_sectors;

		ret = sdmmcCardWriteSectors(&s_sdmcDevNand, &tx, first_sector, cur_sectors);
		if (!ret) {
			break;
		}

		tx.user = (u8*)tx.user + cur_sectors*BLK_SECTOR_SZ;
		first_sector += cur_sectors;
		num_sectors -= cur_sectors;
	}

	return ret;
}
