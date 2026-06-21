// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <string.h>
#include <calico/arm/common.h>
#include <calico/nds/system.h>
#include <calico/nds/bios.h>
#include <calico/nds/tlnc.h>
#ifdef ARM9
#include <calico/arm/cache.h>
#endif

#define g_tlncArea ((TlncArea*)MM_ENV_TWL_AUTOLOAD)
#define g_tlncParamArea ((TlncParamArea*)MM_ENV_TWL_AUTOLOAD_PARAM)

bool tlncGetDataTWL(TlncData* data)
{
	// Check if we can expect the TLNC to exist
	if (!g_envTwlResetFlags->is_valid || !g_envTwlResetFlags->is_warmboot || g_envTwlResetFlags->skip_tlnc) {
		return false;
	}

#ifdef ARM9
	// Invalidate data cache before accessing TLNC
	armDCacheInvalidate(g_tlncArea, sizeof(*g_tlncArea));
#endif

	// Check if the TLNC has a valid header
	if (g_tlncArea->magic != TLNC_MAGIC || g_tlncArea->version != TLNC_VERSION || g_tlncArea->data_len != sizeof(*data)) {
		return false;
	}

	// Check if the TLNC data checksum is correct
	*data = g_tlncArea->data;
	if (svcGetCRC16(0xffff, data, sizeof(*data)) != g_tlncArea->data_crc16) {
		*data = (TlncData){0};
		return false;
	}

	return true;
}

void tlncSetDataTWL(TlncData const* data)
{
	g_tlncArea->magic = TLNC_MAGIC;
	g_tlncArea->version = TLNC_VERSION;
	g_tlncArea->data_len = sizeof(*data);
	g_tlncArea->data_crc16 = svcGetCRC16(0xffff, data, sizeof(*data));
	g_tlncArea->data = *data;

#ifdef ARM9
	// Flush data cache after writing TLNC
	armDCacheFlush(g_tlncArea, sizeof(*g_tlncArea));
#endif
}

bool tlncGetParamTWL(TlncParam* param)
{
	// Check if we can expect the param area to exist
	if (!(g_tlncParamArea->flags & 2)) {
		return false;
	}

	// Check if the param area checksum is correct
	u16 claimed_crc16 = g_tlncParamArea->crc16;
	g_tlncParamArea->crc16 = 0;
	g_tlncParamArea->crc16 = svcGetCRC16(0xffff, g_tlncParamArea, sizeof(*g_tlncParamArea));
	if (claimed_crc16 != g_tlncParamArea->crc16) {
		g_tlncParamArea->crc16 = claimed_crc16;
		return false;
	}

	// Extract information from param area
	param->tid = g_tlncParamArea->caller_tid;
	param->makercode = g_tlncParamArea->caller_makercode;
	param->extra = g_tlncParamArea->extra;
	param->head_len = g_tlncParamArea->head_len;
	param->tail_len = g_tlncParamArea->tail_len;
	param->head = param->head_len ? &g_tlncParamArea->data[0] : NULL;
	param->tail = param->tail_len ? &g_tlncParamArea->data[param->head_len] : NULL;

	return true;
}

void tlncSetParamTWL(TlncParam const* param)
{
	// Clear out param area
	armFillMem32(g_tlncParamArea, 0, sizeof(*g_tlncParamArea));

	// Fast fail if the total data size is too big
	size_t total_len = param->head_len + param->tail_len;
	if (total_len > TLNC_MAX_PARAM_SZ) {
		return;
	}

	// Set basic data in param area
	g_tlncParamArea->caller_tid = param->tid;
	g_tlncParamArea->flags = 3;
	g_tlncParamArea->caller_makercode = param->makercode;
	g_tlncParamArea->head_len = param->head_len;
	g_tlncParamArea->tail_len = param->tail_len;
	g_tlncParamArea->extra = param->extra;

	// Copy head data if needed
	if (g_tlncParamArea->head_len && param->head) {
		memcpy(&g_tlncParamArea->data[0], param->head, param->head_len);
	}

	// Copy tail data if needed
	if (g_tlncParamArea->tail_len && param->tail) {
		memcpy(&g_tlncParamArea->data[param->head_len], param->tail, param->tail_len);
	}

	// Calculate finalized param area checksum
	g_tlncParamArea->crc16 = svcGetCRC16(0xffff, g_tlncParamArea, sizeof(*g_tlncParamArea));

#ifdef ARM9
	// Flush data cache after writing TLNC
	armDCacheFlush(g_tlncParamArea, sizeof(*g_tlncParamArea));
#endif
}

bool tlncSetJumpByArgvTWL(char* const argv[])
{
	char argbuf[TLNC_MAX_PARAM_SZ];
	size_t argbuflen = 0;

	// Create argument string by concatenating null-terminated strings
	for (unsigned i = 0; argv[i]; i ++) {
		const char* arg = argv[i];
		size_t arglen = strlen(arg) + 1;
		if ((TLNC_MAX_PARAM_SZ - argbuflen) < arglen) {
			// Argument buffer overrun
			return false;
		}

		memcpy(&argbuf[argbuflen], arg, arglen);
		argbuflen += arglen;
	}

	// Fail if no argv is actually supplied
	if (!argbuflen) {
		return false;
	}

	// Configure param area
	TlncParam param = {0};
	param.tid = g_envAppTwlHeader->title_id;
	param.makercode = *(u16*)&g_envAppTwlHeader->base.makercode[0];
	param.head_len = argbuflen;
	param.head = argbuf;
	tlncSetParamTWL(&param);

	// Configure TLNC
	TlncData tlnc = {0};
	tlnc.caller_tid = param.tid;
	tlnc.target_tid = 1;
	tlnc.valid = 1;
	tlnc.app_type = TlncAppType_Ram;
	tlnc.skip_intro = 1;
	// Not setting tlnc.skip_load
	tlncSetDataTWL(&tlnc);

	return true;
}
