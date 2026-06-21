// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/system/mailbox.h>
#include <calico/dev/dldi.h>
#include <calico/dev/blk.h>
#include <calico/nds/system.h>
#include <calico/nds/pxi.h>
#include <calico/nds/arm7/twlblk.h>

#include "../crt0.h"
#include "../transfer.h"
#include "../pxi/blkdev.h"

static BlkDevCallbackFn s_blkDevCallback;

static DISC_INTERFACE* s_dldiDiscIface;
static bool s_blkHasTwl;

static Mailbox s_blkPxiMailbox;
static u32 s_blkPxiMailboxData[8];
static Thread s_blkPxiThread;
alignas(8) static u8 s_blkPxiThreadStack[1024];

static int _blkPxiThread(void* unused)
{
	for (;;) {
		u32 msg = mailboxRecv(&s_blkPxiMailbox);

		PxiBlkDevMsgType type = pxiBlkDevMsgGetType(msg);
		u32 imm = pxiBlkDevMsgGetImmediate(msg);
		u32 reply = 0;

		switch (type) {
			default: break;

			case PxiBlkDevMsg_IsPresent:
				reply = blkDevIsPresent((BlkDevice)imm);
				break;

			case PxiBlkDevMsg_Init:
				reply = blkDevInit((BlkDevice)imm);
				break;

			case PxiBlkDevMsg_ReadSectors:
			case PxiBlkDevMsg_WriteSectors: {
				void* buffer = (void*)mailboxRecv(&s_blkPxiMailbox);
				u32 first_sector = mailboxRecv(&s_blkPxiMailbox);
				u32 num_sectors = mailboxRecv(&s_blkPxiMailbox);

				if (type == PxiBlkDevMsg_ReadSectors) {
					reply = blkDevReadSectors((BlkDevice)imm, buffer, first_sector, num_sectors);
				} else /* if (type == PxiBlkDevCmd_WriteSectors) */ {
					reply = blkDevWriteSectors((BlkDevice)imm, buffer, first_sector, num_sectors);
				}
				break;
			}

			case PxiBlkDevMsg_DumpDldi: {
				void* buffer = (void*)mailboxRecv(&s_blkPxiMailbox);
				if (s_dldiDiscIface) {
					DldiHeader* dldi = (DldiHeader*)((u8*)s_dldiDiscIface - offsetof(DldiHeader, disc));
					armCopyMem32(buffer, dldi, 1U << dldi->driver_sz_log2);
					reply = 1;
				}
			}
		}

		pxiReply(PxiChannel_BlkDev, reply);
	}

	return 0;
}

void blkInit(void)
{
	if (systemIsTwlMode()) {
		s_blkHasTwl = twlblkInit();
	}

	mailboxPrepare(&s_blkPxiMailbox, s_blkPxiMailboxData, sizeof(s_blkPxiMailboxData)/sizeof(u32));
	pxiSetMailbox(PxiChannel_BlkDev, &s_blkPxiMailbox);
	threadPrepare(&s_blkPxiThread, _blkPxiThread, NULL, &s_blkPxiThreadStack[sizeof(s_blkPxiThreadStack)], THREAD_MIN_PRIO-1);
	threadStart(&s_blkPxiThread);
}

void blkSetDevCallback(BlkDevCallbackFn fn)
{
	s_blkDevCallback = fn;
}

MK_NOINLINE bool blkDevIsPresent(BlkDevice dev)
{
	switch (dev) {
		default:
			return false;

		case BlkDevice_Dldi:
			return s_dldiDiscIface && s_dldiDiscIface->isInserted();

		case BlkDevice_TwlSdCard:
			return s_blkHasTwl && twlSdIsInserted();

		case BlkDevice_TwlNand:
		case BlkDevice_TwlNandAes:
			return s_blkHasTwl;
	}
}

MK_NOINLINE bool blkDevInit(BlkDevice dev)
{
	switch (dev) {
		default:
			return false;

		case BlkDevice_Dldi: {
			bool rc = s_dldiDiscIface && s_dldiDiscIface->startup();
			if (rc) {
				// XX: detect disc size using MBR
				s_transferRegion->blkdev_sector_count[BlkDevice_Dldi] = UINT32_MAX;
			}
			return rc;
		}

		case BlkDevice_TwlSdCard:
			return s_blkHasTwl && twlSdInit();

		case BlkDevice_TwlNand:
		case BlkDevice_TwlNandAes:
			return s_blkHasTwl && twlNandInit();
	}

	return false;
}

MK_NOINLINE u32 blkDevGetSectorCount(BlkDevice dev)
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

MK_NOINLINE bool blkDevReadSectors(BlkDevice dev, void* buffer, u32 first_sector, u32 num_sectors)
{
	switch (dev) {
		default:
			return false;

		case BlkDevice_Dldi:
			return s_dldiDiscIface && s_dldiDiscIface->readSectors(first_sector, num_sectors, buffer);

		case BlkDevice_TwlSdCard:
			return s_blkHasTwl && twlSdReadSectors(buffer, first_sector, num_sectors);

		case BlkDevice_TwlNand:
			return s_blkHasTwl && twlNandReadSectors(buffer, first_sector, num_sectors);

		case BlkDevice_TwlNandAes:
			return s_blkHasTwl && twlNandReadSectorsAes(buffer, first_sector, num_sectors);
	}
}

MK_NOINLINE bool blkDevWriteSectors(BlkDevice dev, const void* buffer, u32 first_sector, u32 num_sectors)
{
	switch (dev) {
		default:
			return false;

		case BlkDevice_Dldi:
			return s_dldiDiscIface && s_dldiDiscIface->writeSectors(first_sector, num_sectors, buffer);

		case BlkDevice_TwlSdCard:
			return s_blkHasTwl && twlSdWriteSectors(buffer, first_sector, num_sectors);

		case BlkDevice_TwlNand:
			return s_blkHasTwl && twlNandWriteSectors(buffer, first_sector, num_sectors);

		case BlkDevice_TwlNandAes:
			return s_blkHasTwl && twlNandWriteSectorsAes(buffer, first_sector, num_sectors);
	}
}

void _blkShelterDldi(void)
{
	// Check if we have a valid ARM9 module header
	Crt0Header9* mod9 = (Crt0Header9*)(g_envAppNdsHeader->arm9_entrypoint + 4);
	if (!crt0IsValidHeader(&mod9->base, CRT0_MAGIC_ARM9)) {
		return;
	}

	// Check the ARM9 module contains a valid DLDI driver
	uptr dldi_lma = mod9->dldi_addr;
	if (dldi_lma < g_envAppNdsHeader->arm9_ram_address || dldi_lma >= g_envAppNdsHeader->arm9_ram_address + g_envAppNdsHeader->arm9_size) {
		return;
	}

	// Check the DLDI driver is not a stub driver
	DldiHeader* dldi_hdr = (DldiHeader*)dldi_lma;
	u32 dldi_features = dldi_hdr->disc.features;
	if (!(dldi_features & DLDI_FEATURE_CAN_READ)) {
		return;
	}

	// Check the DLDI driver does not overlap used WRAM
	extern char __wram_bss_end[];
	extern char __sys_start[];
	uptr avail_sz = (uptr)__sys_start - dldi_hdr->dldi_start;
	uptr dldi_sz = 1U << dldi_hdr->driver_sz_log2;
	if (dldi_hdr->dldi_start < (uptr)__wram_bss_end || dldi_hdr->dldi_start >= (uptr)__sys_start || dldi_sz > avail_sz) {
		return;
	}

	// Move DLDI to WRAM
	crt0CopyMem32(dldi_hdr->dldi_start, dldi_lma, dldi_sz);

	// Remember pointer to DLDI disc interface
	s_dldiDiscIface = &((DldiHeader*)dldi_hdr->dldi_start)->disc;

	// Expose info about the loaded DLDI driver
	g_envExtraInfo->dldi_features = dldi_features;
	g_envExtraInfo->dldi_io_type  = s_dldiDiscIface->ioType;

	// Declare which EXMEMCNT bits we need for DLDI access on the ARM7
	u16 exmemcnt_bits = 0;
	if (dldi_features & DLDI_FEATURE_SLOT_GBA) {
		exmemcnt_bits |= EXMEMCNT_GBA_SLOT_ARM7;
	}
	if (dldi_features & DLDI_FEATURE_SLOT_NDS) {
		exmemcnt_bits |= EXMEMCNT_NDS_SLOT_ARM7;
	}
	s_transferRegion->exmemcnt_mirror |= exmemcnt_bits;
}
