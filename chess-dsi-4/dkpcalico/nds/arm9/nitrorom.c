// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <fcntl.h>
#include <unistd.h>

#include <calico/system/mutex.h>
#include <calico/nds/env.h>
#include <calico/nds/ntrcard.h>
#include <calico/nds/nitrorom.h>

typedef struct NitroRomFd {
	Mutex mutex;
	int fd;
	u32 pos;
} NitroRomFd;

MK_WEAK s8 g_nitroromCardDmaChannel = -1;

static NitroRom s_nitroromSelf;
static NitroRomFd s_nitroromFd;

MK_INLINE bool _nitroromFdReadImpl(NitroRomFd* self, u32 offset, void* buf, u32 size)
{
	if (self->pos != offset) {
		if (lseek(self->fd, offset, SEEK_SET) < 0) {
			return false;
		}
		self->pos = offset;
	}

	ssize_t bytes = read(self->fd, buf, size);
	if (bytes < 0) {
		return false;
	}

	self->pos += bytes;
	if (bytes != size) {
		return false;
	}

	return true;
}

static bool _nitroromFdRead(void* user, u32 offset, void* buf, u32 size)
{
	NitroRomFd* self = user;
	mutexLock(&self->mutex);
	bool ok = _nitroromFdReadImpl(self, offset, buf, size);
	mutexUnlock(&self->mutex);
	return ok;
}

static void _nitroromFdClose(void* user)
{
	NitroRomFd* self = user;
	close(self->fd);
}

static const NitroRomIface s_nitroromFdIface = {
	.read  = _nitroromFdRead,
	.close = _nitroromFdClose,
};

static const NitroRomIface s_nitroromCardIface = {
	.read  = (void*)ntrcardRomRead,
	.close = (void*)ntrcardClose,
};

NitroRom* nitroromGetSelf(void)
{
	// Succeed early if NitroROM is already open
	if_likely (s_nitroromSelf.iface) {
		return &s_nitroromSelf;
	}

	// Use FAT/FNT parameters from NDS header
	NitroRomParams params = {
		.fat_offset = g_envAppNdsHeader->fat_rom_offset,
		.fat_sz     = g_envAppNdsHeader->fat_size,
		.fnt_offset = g_envAppNdsHeader->fnt_rom_offset,
		.fnt_sz     = g_envAppNdsHeader->fnt_size,
		.img_offset = 0,
	};

	bool ok = false;

	// Attempt to open argv[0] if provided
	const char* argv0 = g_envNdsArgvHeader->argv[0];
	if (argv0) {
		int fd = open(argv0, O_RDONLY);
		if (fd >= 0) {
			s_nitroromFd.fd = fd;
			s_nitroromFd.pos = 0;
			ok = nitroromOpen(&s_nitroromSelf, &params, &s_nitroromFdIface, &s_nitroromFd);
			if (!ok) {
				close(fd);
			}
		}
	}

	// If above failed and we're booted from a real NDS card (e.g. emulators), try direct card access
	if (!ok && !argv0 && g_envBootParam->boot_src == EnvBootSrc_Card) {
		ok = ntrcardOpen();
		if (ok) {
			ok = nitroromOpen(&s_nitroromSelf, &params, &s_nitroromCardIface, (void*)(int)g_nitroromCardDmaChannel);
			if (!ok) {
				ntrcardClose();
			}
		}
	}

	return ok ? &s_nitroromSelf : NULL;
}
