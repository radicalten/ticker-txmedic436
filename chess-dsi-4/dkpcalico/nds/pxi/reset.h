// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#pragma once
#include <calico/nds/pxi.h>

#define PXI_LIBNDS_FIFO_DATAMSG (0U << 20)
#define PXI_LIBNDS_FIFO_VALUE32 (1U << 20)
#define PXI_LIBNDS_FIFO_ADDRESS (2U << 20)
#define PXI_LIBNDS_FIFO_SPECIAL (3U << 20)

typedef enum PxiResetMsgType {
	PxiResetMsgType_Reset = 0x10,
	PxiResetMsgType_Abort = 0x20,
} PxiResetMsgType;

MK_CONSTEXPR u32 pxiResetMakeMsg(PxiResetMsgType type)
{
	return ((type & 0x7f) << 8) | PXI_LIBNDS_FIFO_SPECIAL;
}

MK_CONSTEXPR PxiResetMsgType pxiResetGetType(u32 msg)
{
	return (PxiResetMsgType)((msg >> 8) & 0x7f);
}
