// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/irq.h>
#include <calico/system/thread.h>
#include <calico/nds/arm7/spi.h>
#include <calico/nds/arm7/pmic.h>

static void _pmicSpiBegin(u8 cmd)
{
	spiRawStartHold(SpiDev_PMIC, SpiBaud_1MHz);
	spiRawWriteByte(cmd);
	spiRawEndHold(SpiDev_PMIC, SpiBaud_1MHz);
}

bool pmicWriteRegister(PmicRegister reg, u8 data)
{
	if (!mutexIsLockedByCurrentThread(&g_spiMutex)) {
		return false;
	}

	_pmicSpiBegin(reg);
	spiRawWriteByte(data);
	return true;
}

u8 pmicReadRegister(PmicRegister reg)
{
	if (!mutexIsLockedByCurrentThread(&g_spiMutex)) {
		return 0xff;
	}

	_pmicSpiBegin(reg | (1U<<7));
	return spiRawReadByte();
}

void pmicIssueShutdown(void)
{
	armIrqLockByPsr();
	spiLock();
	pmicWriteRegister(PmicReg_Control, PMIC_CTRL_SHUTDOWN);
	for (;;); // infinite loop just in case
}
