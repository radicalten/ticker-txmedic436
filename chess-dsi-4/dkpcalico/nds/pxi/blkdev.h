// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/dev/blk.h>
#include <calico/nds/pxi.h>

typedef enum PxiBlkDevMsgType {
	// ARM9 -> ARM7
	PxiBlkDevMsg_IsPresent    = 0,
	PxiBlkDevMsg_Init         = 1,
	PxiBlkDevMsg_ReadSectors  = 2,
	PxiBlkDevMsg_WriteSectors = 3,
	PxiBlkDevMsg_DumpDldi     = 4,

	// ARM7 -> ARM9
	PxiBlkDevMsg_Removed      = 0x1e,
	PxiBlkDevMsg_Inserted     = 0x1f,
} PxiBlkDevMsgType;

MK_CONSTEXPR u32 pxiBlkDevMakeMsg(PxiBlkDevMsgType type, unsigned imm)
{
	return (type & 0x1f) | ((imm & 0x7ff) << 5);
}

MK_CONSTEXPR PxiBlkDevMsgType pxiBlkDevMsgGetType(u32 msg)
{
	return (PxiBlkDevMsgType)(msg & 0x1f);
}

MK_CONSTEXPR unsigned pxiBlkDevMsgGetImmediate(u32 msg)
{
	return (msg >> 5) & 0x7ff;
}
