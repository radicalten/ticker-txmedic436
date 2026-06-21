// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <string.h>
#include <calico/types.h>
#include <calico/system/mutex.h>
#include <calico/nds/pxi.h>
#include <calico/nds/arm7/debug.h>
#include "../transfer.h"

DebugBufferMode g_dbgBufMode = DbgBufMode_Line;
static Mutex s_debugMutex;

void debugOutput(const char* buf, size_t size)
{
	mutexLock(&s_debugMutex);

	while (size) {
		while ((s_debugBuf->flags & (DBG_BUF_ALIVE|DBG_BUF_BUSY)) != DBG_BUF_ALIVE) {
			pxiWaitForPing();
		}

		u32 remaining_sz = sizeof(s_debugBuf->data) - s_debugBuf->size;
		u32 this_size = size > remaining_sz ? remaining_sz : size;

		bool should_ping = g_dbgBufMode == DbgBufMode_None || this_size == remaining_sz;
		char* outbuf = s_debugBuf->data + s_debugBuf->size;
		if_likely (buf) {
			for (u32 i = 0; i < this_size; i ++) {
				outbuf[i] = buf[i];
				if_unlikely (buf[i] == '\n' && g_dbgBufMode == DbgBufMode_Line) {
					// Flush on newline
					should_ping = true;
					this_size = i + 1;
				}
			}
		} else {
			// Hack, so that printing padding spaces through dietPrint is more efficient
			memset(outbuf, ' ', this_size);
		}

		s_debugBuf->size += this_size;
		buf += this_size;
		size -= this_size;

		if (should_ping) {
			s_debugBuf->flags |= DBG_BUF_BUSY;
			pxiPing();
		}
	}

	mutexUnlock(&s_debugMutex);
}
