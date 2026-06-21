// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/irq.h>
#include <calico/system/thread.h>
#include <calico/nds/arm7/spi.h>
#include <calico/nds/arm7/nvram.h>

static void _nvramSpiBegin(NvramCmd cmd)
{
	spiRawStartHold(SpiDev_NVRAM, SpiBaud_4MHz);
	spiRawWriteByte(cmd);
}

MK_INLINE void _nvramSpiPreEnd(void)
{
	spiRawEndHold(SpiDev_NVRAM, SpiBaud_4MHz);
}

static u8 _nvramReadStatus(void)
{
	_nvramSpiBegin(NvramCmd_ReadStatus);
	_nvramSpiPreEnd();
	return spiRawReadByte();
}

bool nvramWaitReady(void)
{
	if (!mutexIsLockedByCurrentThread(&g_spiMutex)) {
		return false;
	}

	while (_nvramReadStatus() & NVRAM_STATUS_WIP);

	return true;
}

bool nvramReadJedec(u32* out)
{
	if (!mutexIsLockedByCurrentThread(&g_spiMutex)) {
		return false;
	}

	_nvramSpiBegin(NvramCmd_ReadJedec);
	unsigned hi = spiRawReadByte();
	unsigned mid = spiRawReadByte();
	_nvramSpiPreEnd();
	unsigned lo = spiRawReadByte();

	*out = lo | (mid<<8) | (hi<<16);
	return true;
}

bool nvramReadDataBytes(void* data, u32 addr, u32 len)
{
	if (!len) {
		return true;
	}

	if (!mutexIsLockedByCurrentThread(&g_spiMutex)) {
		return false;
	}

	_nvramSpiBegin(NvramCmd_ReadDataBytes);
	spiRawWriteByte(addr >> 16);
	spiRawWriteByte(addr >> 8);
	spiRawWriteByte(addr);

	u8* data8 = (u8*)data;
	for (u32 i = 0; i < len - 1; i ++) {
		*data8++ = spiRawReadByte();
	}

	_nvramSpiPreEnd();
	*data8++ = spiRawReadByte();

	return true;
}
