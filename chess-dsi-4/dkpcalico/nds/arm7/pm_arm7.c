// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/nds/system.h>
#include <calico/nds/env.h>
#include <calico/nds/arm7/pmic.h>
#include <calico/nds/arm7/nvram.h>
#include <calico/nds/arm7/codec.h>
#include <calico/nds/arm7/i2c.h>
#include <calico/nds/arm7/mcu.h>
#include <calico/nds/pm.h>

unsigned pmGetBatteryState(void)
{
	unsigned ret;

	if (systemIsTwlMode()) {
		// DSi mode: Read state from MCU
		// XX: Official code converts 0-15 into 0-5 with the following formula:
		//     (level&1) + ((level&2)>>1) + ((level&0xc)>>2)
		i2cLock();
		ret = i2cReadRegister8(I2cDev_MCU, McuReg_BatteryState);
		i2cUnlock();
	} else {
		// DS mode: Read state from PMIC
		spiLock();
		ret = (pmicReadRegister(PmicReg_BatteryStatus) & PMIC_BATT_STAT_LOW) ? 3 : 15;
		if (g_envExtraInfo->nvram_console_type & EnvConsoleType_DSLite) {
			// If on DS Lite: read charger status too
			ret |= (pmicReadRegister(PmicReg_BacklightLevel) & PMIC_BL_CHARGER_DETECTED) ? PM_BATT_CHARGING : 0;
		}
		spiUnlock();
	}

	return ret;
}

void pmSetPowerLed(PmLedMode mode)
{
	spiLock();
	unsigned reg = pmicReadRegister(PmicReg_Control);
	pmicWriteRegister(PmicReg_Control, (reg&~PMIC_CTRL_LED_MASK) | ((mode&3)<<4));
	spiUnlock();
}

void pmSoundSetAmpPower(bool enable)
{
	spiLock();
	if (cdcIsTwlMode()) {
		// TODO: implement this with CODEC
	} else {
		// Use PMIC
		unsigned reg = pmicReadRegister(PmicReg_Control);
		if (enable) {
			reg = (reg &~ PMIC_CTRL_SOUND_MUTE) | PMIC_CTRL_SOUND_ENABLE;
		} else {
			reg = (reg &~ PMIC_CTRL_SOUND_ENABLE) | PMIC_CTRL_SOUND_MUTE;
		}
		pmicWriteRegister(PmicReg_Control, reg);
	}
	spiUnlock();
}

MK_CONSTEXPR PmicMicGain _pmMicGainToPmic(unsigned gain)
{
	if (gain < (PmMicGain_20+PmMicGain_40)/2) {
		return PmicMicGain_20;
	} else if (gain < (PmMicGain_40+PmMicGain_80)/2) {
		return PmicMicGain_40;
	} else if (gain < (PmMicGain_80+PmMicGain_160)/2) {
		return PmicMicGain_80;
	} else {
		return PmicMicGain_160;
	}
}

void pmMicSetAmp(bool enable, unsigned gain)
{
	if (gain > PmMicGain_Max) {
		gain = PmMicGain_Max;
	}

	spiLock();
	if (cdcIsTwlMode()) {
		// Use CODEC
		cdcMicSetAmp(enable, gain);
	} else {
		// Use PMIC
		pmicWriteRegister(PmicReg_MicAmpControl, enable ? 1 : 0);
		if (enable) {
			pmicWriteRegister(PmicReg_MicAmpGain, _pmMicGainToPmic(gain));
		}
	}
	spiUnlock();
}

bool pmReadNvram(void* data, u32 addr, u32 len)
{
	spiLock();
	bool rc = nvramReadDataBytes(data, addr, len);
	spiUnlock();
	return rc;
}
