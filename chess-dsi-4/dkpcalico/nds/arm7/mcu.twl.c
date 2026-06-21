// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/irq.h>
#include <calico/system/thread.h>
#include <calico/nds/arm7/pmic.h>
#include <calico/nds/arm7/i2c.h>
#include <calico/nds/arm7/mcu.h>

#define MCU_IRQ_MASK ( \
	MCU_IRQ_PWRBTN_RESET|MCU_IRQ_PWRBTN_SHUTDOWN|MCU_IRQ_PWRBTN_BEGIN| \
	MCU_IRQ_BATTERY_EMPTY|MCU_IRQ_BATTERY_LOW|MCU_IRQ_VOLBTN \
)

void _i2cSetMcuDelay(s32 delay);

McuPwrBtnState g_mcuPwrBtnState;

static Thread s_mcuThread;
static alignas(8) u8 s_mcuThreadStack[0x200];

static McuIrqHandler s_mcuIrqTable[8];

static unsigned _mcuCheckIrqFlags(void)
{
	i2cLock();
	unsigned flags = i2cReadRegister8(I2cDev_MCU, McuReg_IrqFlags);
	i2cUnlock();
	return flags;
}

MK_INLINE bool _mcuIrqMaskUnpack(unsigned* pmask, unsigned* pid)
{
	if (!*pmask)
		return false;
	while (!(*pmask & (1U << *pid)))
		++*pid;
	*pmask &= ~(1U << *pid);
	return true;
}

static int _mcuThread(void* unused)
{
	// Enable MCU interrupt
	irqEnable2(IRQ2_MCU);

	for (;;) {
		unsigned flags = _mcuCheckIrqFlags();

		// Update power button state
		if (flags & MCU_IRQ_PWRBTN_SHUTDOWN) {
			g_mcuPwrBtnState = McuPwrBtnState_Shutdown;
		} else if (flags & MCU_IRQ_PWRBTN_RESET) {
			g_mcuPwrBtnState = McuPwrBtnState_Reset;
		} else if (flags & MCU_IRQ_PWRBTN_BEGIN) {
			g_mcuPwrBtnState = McuPwrBtnState_Begin;
		}

		// Call irq handlers
		unsigned id = 0;
		while (_mcuIrqMaskUnpack(&flags, &id)) {
			if (s_mcuIrqTable[id]) {
				s_mcuIrqTable[id](1U << id);
			}
		}

		threadIrqWait2(false, IRQ2_MCU);
	}

	return 0;
}

void mcuInit(void)
{
	// Retrieve MCU firmware version, and set up the correct I2C delay for it
	i2cLock();
	unsigned mcuVersion = i2cReadRegister8(I2cDev_MCU, McuReg_Version);
	i2cUnlock();
	if (mcuVersion <= 0x20) {
		_i2cSetMcuDelay(0x180);
	} else {
		_i2cSetMcuDelay(0x90);
	}
}

void mcuStartThread(u8 thread_prio)
{
	// Set up MCU thread
	threadPrepare(&s_mcuThread, _mcuThread, NULL, &s_mcuThreadStack[sizeof(s_mcuThreadStack)], thread_prio);
	threadStart(&s_mcuThread);
}

void mcuIrqSet(unsigned irq_mask, McuIrqHandler fn)
{
	IrqState st = irqLock();
	unsigned id = 0;
	irq_mask &= MCU_IRQ_MASK;
	while (_mcuIrqMaskUnpack(&irq_mask, &id)) {
		s_mcuIrqTable[id] = fn;
	}
	irqUnlock(st);
}

void mcuIssueReset(void)
{
	armIrqLockByPsr();
	i2cLock();
	i2cWriteRegister8(I2cDev_MCU, McuReg_WarmbootFlag, 1);
	i2cWriteRegister8(I2cDev_MCU, McuReg_DoReset, 1);
	for (;;); // infinite loop just in case
}

void mcuIssueShutdown(void)
{
	armIrqLockByPsr();
	i2cLock();
	i2cWriteRegister8(I2cDev_MCU, McuReg_DoReset, 2);
	pmicIssueShutdown();
}
