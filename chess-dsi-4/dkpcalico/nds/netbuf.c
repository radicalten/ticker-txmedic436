// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/dev/netbuf.h>
#include <calico/nds/mm.h>
#include <calico/nds/mm_env.h>
#include <calico/nds/smutex.h>

#ifdef ARM9
#include <calico/arm/cache.h>

MK_INLINE void _netbufFlushHdr(NetBuf* nb)
{
	// Flush only the header (single cache line)
	armDCacheFlush(nb, sizeof(NetBuf));
}

#else
#define _netbufFlushHdr(...) ((void)0)
#endif

#define NETBUF_NUM_SUBPOOLS 5

#define s_netbufMgr ((_NetbufMgr*) MM_ENV_FREE_FCF0)

static const u16 s_netbufListSizes[NETBUF_NUM_SUBPOOLS] = { 128, 256, 512, 1024, 2048 };

typedef struct _NetBufPool {
	SMutex lock;
	NetBufListNode subpools[NETBUF_NUM_SUBPOOLS];
} _NetBufPool;

typedef struct _NetbufMgr {
	_NetBufPool pools[NetBufPool_Count];
} _NetbufMgr;

MK_CONSTEXPR unsigned _netbufFindSubpool(unsigned size)
{
	unsigned i;
	for (i = 0; i < NETBUF_NUM_SUBPOOLS; i ++) {
		if (s_netbufListSizes[i] >= size) {
			break;
		}
	}
	return i;
}

MK_INLINE void _netbufEnqueue(NetBufListNode* subpool, NetBuf* nb)
{
	NetBuf* pos = subpool->prev;
	if (pos) {
		pos->link.next = nb;
		_netbufFlushHdr(pos);
		nb->link.prev = pos;
	} else {
		subpool->next = nb;
	}
	nb->link.next = NULL;
	nb->link.prev = pos;
	subpool->prev = nb;
	_netbufFlushHdr(nb);
}

MK_INLINE NetBuf* _netbufDequeue(NetBufListNode* subpool)
{
	NetBuf* nb = subpool->next;
	if (nb) {
		NetBuf* nbnext = nb->link.next;
		subpool->next = nbnext;
		if (nbnext) {
			nbnext->link.prev = NULL;
			_netbufFlushHdr(nbnext);
		} else {
			subpool->prev = NULL;
		}
	}
	return nb;
}

static NetBuf* _netbufPoolAlloc(_NetBufPool* p, unsigned hdr_headroom_sz, unsigned data_sz)
{
	unsigned i;
	for (i = _netbufFindSubpool(hdr_headroom_sz+data_sz); i < NETBUF_NUM_SUBPOOLS; i ++) {
		NetBufListNode* subpool = &p->subpools[i];
		NetBuf* nb = _netbufDequeue(subpool);
		if (nb) {
			nb->pos = hdr_headroom_sz;
			nb->len = data_sz;
			return nb;
		}
	}
	return NULL;
}

static void _netbufPoolFree(_NetBufPool* p, NetBuf* nb)
{
	NetBufListNode* subpool = (NetBufListNode*)nb->system[1];
	if (subpool) {
		_netbufEnqueue(subpool, nb);
	}
}

static void* _netbufPoolInit(_NetBufPool* p, void* start, const u16* counts)
{
	for (unsigned i = 0; i < NETBUF_NUM_SUBPOOLS; i ++) {
		NetBufListNode* subpool = &p->subpools[i];
		for (unsigned j = 0; j < counts[i]; j ++) {
			NetBuf* nb = (NetBuf*)start;
			*nb = (NetBuf){0};
			nb->capacity = s_netbufListSizes[i];
			nb->system[0] = (uptr)p;
			nb->system[1] = (uptr)subpool;
			start = (u8*)(nb+1) + nb->capacity;
			_netbufEnqueue(subpool, nb);
		}
	}
	return start;
}

void _netbufPrvInitPools(void* start, const u16* tx_counts, const u16* rx_counts)
{
	_NetBufPool* txp = &s_netbufMgr->pools[NetBufPool_Tx];
	_NetBufPool* rxp = &s_netbufMgr->pools[NetBufPool_Rx];

	smutexLock(&txp->lock);
	start = _netbufPoolInit(txp, start, tx_counts);
	smutexUnlock(&txp->lock);

	smutexLock(&rxp->lock);
	start = _netbufPoolInit(rxp, start, rx_counts);
	smutexUnlock(&rxp->lock);
}

NetBuf* netbufAlloc(unsigned hdr_headroom_sz, unsigned data_sz, NetBufPool pool)
{
	_NetBufPool* p = &s_netbufMgr->pools[pool];
	smutexLock(&p->lock);
	NetBuf* nb = _netbufPoolAlloc(p, hdr_headroom_sz, data_sz);
	smutexUnlock(&p->lock);
	return nb;
}

void netbufFlush(NetBuf* nb)
{
#ifdef ARM9
	// Flush the entire buffer (including data area)
	armDCacheFlush(nb, sizeof(NetBuf) + nb->capacity);
#endif
}

void netbufFree(NetBuf* nb)
{
	if (!nb) {
		return; // NOP
	}

	_NetBufPool* p = (_NetBufPool*)nb->system[0];
	if (!p) {
		// Do nothing if this netbuf isn't managed by us
		return;
	}

#ifdef ARM9
	// Discard cache lines falling within the data area
	armDCacheInvalidate(nb+1, nb->capacity);
#endif

	smutexLock(&p->lock);
	_netbufPoolFree(p, nb);
	smutexUnlock(&p->lock);
}
