// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/arm/common.h>
#include <calico/nds/env.h>
#include <calico/nds/bios.h>
#include <calico/nds/arm7/nvram.h>

//#define ENV_DEBUG

#ifdef ENV_DEBUG
#include <calico/system/dietprint.h>
#else
#define dietPrint(...) ((void)0)
#endif

MK_INLINE unsigned _envGetUserSettingsNvramOffset(void)
{
	u16 off_div8 = g_envExtraInfo->nvram_offset_div8;
	if (!off_div8) {
		nvramReadDataBytes(&off_div8, 0x20, sizeof(u16));
		g_envExtraInfo->nvram_offset_div8 = off_div8;
		nvramReadDataBytes(&g_envExtraInfo->nvram_console_type, 0x1d, sizeof(u8));

		if (g_envExtraInfo->nvram_console_type == 0xff) {
			g_envExtraInfo->nvram_console_type = EnvConsoleType_DS;
		}
	}
	return off_div8*8;
}

static bool _envValidateUserSettings(EnvUserSettingsNvram* cfg)
{
	u16 calc_crc16 = svcGetCRC16(0xffff, &cfg->base, sizeof(cfg->base));
	if (cfg->crc16 != calc_crc16) {
		dietPrint("[NVRAM] CRC16 %.4X != %.4X\n", cfg->crc16, calc_crc16);
		return false;
	}

	if (cfg->base.version != ENV_USER_SETTINGS_VERSION) {
		dietPrint("[NVRAM] Bad version %u\n", cfg->base.version);
		return false;
	}

	u16 calc_ext_crc16 = svcGetCRC16(0xffff, &cfg->ext, sizeof(cfg->ext));
	if (cfg->ext_crc16 == calc_ext_crc16) {
		dietPrint("[NVRAM] Has ext\n");
		if (cfg->ext.version != ENV_USER_SETTINGS_VERSION_EXT) {
			dietPrint("[NVRAM] Bad ext version %u\n", cfg->ext.version);
		} else {
			// Copy language info
			cfg->base.config.language = cfg->ext.language;
		}
	}

	return true;
}

static void _envLoadUserSettings(void)
{
	EnvUserSettingsNvram cfg[2];

	unsigned user_off = _envGetUserSettingsNvramOffset();
	dietPrint("NVRAM off = %.4X\n", user_off);

	nvramReadDataBytes(cfg, user_off, sizeof(cfg));

	bool ok0 = _envValidateUserSettings(&cfg[0]);
	bool ok1 = _envValidateUserSettings(&cfg[1]);

	EnvUserSettingsNvram* chosen_cfg = NULL;
	if (ok0 && ok1) {
		// Conflict, decide which to copy
		dietPrint("[NVRAM] choosing %.2X %.2X\n", cfg[0].counter, cfg[1].counter);
		chosen_cfg = &cfg[((cfg[0].counter + 1) & 0x7f) == cfg[1].counter];
	} else if (ok0) {
		chosen_cfg = &cfg[0];
	} else if (ok1) {
		chosen_cfg = &cfg[1];
	}

	if (chosen_cfg) {
		dietPrint("[NVRAM] Chosen slot %u\n", chosen_cfg-cfg);
		armCopyMem32(g_envUserSettings, &chosen_cfg->base, sizeof(EnvUserSettings));
	} else {
		dietPrint("[NVRAM] No settings loaded!\n");
		armFillMem32(g_envUserSettings, 0, sizeof(EnvUserSettings));
	}
}

void envReadNvramSettings(void)
{
	spiLock();
	nvramWaitReady();
	_envLoadUserSettings();
	spiUnlock();
}
