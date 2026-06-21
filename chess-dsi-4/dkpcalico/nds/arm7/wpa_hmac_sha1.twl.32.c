// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/tick.h>
#include <calico/dev/wpa.h>
#include <calico/nds/lcd.h>
#include <calico/nds/bios.h>
#include "../transfer.h"

void wpaHmacSha1(void* out, const void* key, size_t key_len, const void* data, size_t data_len)
{
	SvcSha1Context ctx;
	ctx.hash_block = NULL;

	u8 keyblock[64];
	const u8* keyp = (const u8*)key;

	// Clamp down key length when larger than block size
	if (key_len > sizeof(keyblock)) {
		svcSha1CalcTWL(keyblock, key, key_len);
		keyp = keyblock;
		key_len = SVC_SHA1_DIGEST_SZ;
	}

	// Copy key (zeropadded) and XOR it with 0x36
	size_t i;
	for (i = 0; i < key_len; i ++) {
		keyblock[i] = keyp[i] ^ 0x36;
	}
	for (; i < sizeof(keyblock); i ++) {
		keyblock[i] = 0x36;
	}

	// Inner hash
	svcSha1InitTWL(&ctx);
	svcSha1UpdateTWL(&ctx, keyblock, sizeof(keyblock));
	svcSha1UpdateTWL(&ctx, data, data_len);
	svcSha1DigestTWL(out, &ctx);

	// Convert inner key into outer key
	for (i = 0; i < sizeof(keyblock); i ++) {
		keyblock[i] ^= 0x36 ^ 0x5c;
	}

	// Outer hash
	svcSha1InitTWL(&ctx);
	svcSha1UpdateTWL(&ctx, keyblock, sizeof(keyblock));
	svcSha1UpdateTWL(&ctx, out, SVC_SHA1_DIGEST_SZ);
	svcSha1DigestTWL(out, &ctx);
}

void wpaGenerateEapolNonce(void* out)
{
	u32 data[8];
	data[0] = *(u32*)&g_envExtraInfo->wlmgr_macaddr[0];
	data[1] = *(u16*)&g_envExtraInfo->wlmgr_macaddr[4] | (lcdGetVCount()<<16);
	data[2] = g_envAppNdsHeader->arm9_size;
	data[3] = g_envAppNdsHeader->arm7_size;
	data[4] = g_envAppTwlHeader->arm9i_size;
	data[5] = g_envAppTwlHeader->arm7i_size;
	data[6] = s_transferRegion->unix_time;
	data[7] = tickGetCount();

	u32* inner = (u32*)out + (WPA_EAPOL_NONCE_LEN - SVC_SHA1_DIGEST_SZ)/4;
	svcSha1CalcTWL(inner, data, sizeof(data));

	for (unsigned i = 0; i < 8; i ++) {
		data[i] ^= inner[i&1];
	}

	svcSha1CalcTWL(out, data, sizeof(data));
}
