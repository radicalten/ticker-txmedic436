// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <string.h>
#include <calico/types.h>
#include <calico/arm/cache.h>
#include <calico/system/thread.h>
#include <calico/system/mailbox.h>
#include <calico/nds/pm.h>
#include <calico/nds/pxi.h>
#include <calico/nds/wlmgr.h>
#include "../transfer.h"
#include "../pxi/wlmgr.h"

#define WLMGR_NUM_MAIL_SLOTS 4
#define WLMGR_RSSI_BUF_SZ    32

void _netbufPrvInitPools(void* start, const u16* tx_counts, const u16* rx_counts);

static Thread s_wlmgrThread;
alignas(8) static u8 s_wlmgrThreadStack[2048];

alignas(ARM_CACHE_LINE_SZ)
static u8 s_wlmgrDefaultPacketHeap[WLMGR_MIN_PACKET_MEM_SZ + 7*(sizeof(NetBuf) + 2048)];

const WlMgrInitConfig g_wlmgrDefaultConfig = {
	.pktmem    = s_wlmgrDefaultPacketHeap,
	.pktmem_sz = sizeof(s_wlmgrDefaultPacketHeap),
	.pktmem_allocmap = (1U<<3)-1,
};

static struct {
	WlMgrState state;
	bool cmd_fail;
	u8 rssi_pos;
	NetBufListNode rx_queue;

	WlMgrEventFn event_cb;
	void* event_user;

	WlMgrRawRxFn rx_cb;
	void* rx_user;

	WlanBssDesc* scan_buf;

	u8 rssi_buf[WLMGR_RSSI_BUF_SZ];
} s_wlmgrState;

static void _wlmgrRssiBufInit(unsigned rssi)
{
	s_wlmgrState.rssi_pos = 0;
	for (unsigned i = 0; i < WLMGR_RSSI_BUF_SZ; i ++) {
		s_wlmgrState.rssi_buf[i] = rssi;
	}
}

static void _wlmgrRssiBufUpdate(unsigned rssi)
{
	s_wlmgrState.rssi_buf[s_wlmgrState.rssi_pos++] = rssi;
	s_wlmgrState.rssi_pos %= WLMGR_RSSI_BUF_SZ;
}

static unsigned _wlmgrRssiBufDigest(void)
{
	unsigned ret = 0;
	for (unsigned i = 0; i < WLMGR_RSSI_BUF_SZ; i ++) {
		ret += s_wlmgrState.rssi_buf[i];
	}
	return ret / WLMGR_RSSI_BUF_SZ;
}

static void _wlmgrRxPxiHandler(void* user, u32 data)
{
	// Calculate packet address
	NetBuf* buf = (NetBuf*)(MM_MAINRAM + (data << 5));

	// Append packet to rx queue
	netbufQueueAppend(&s_wlmgrState.rx_queue, buf);

	// Signal thread if needed
	Mailbox* mbox = (Mailbox*)user;
	if (mbox->pending_slots == 0) {
		mailboxTrySend(mbox, UINT32_MAX);
	}
}

static int _wlmgrThreadMain(void* arg)
{
	// Set up main mailbox
	Mailbox mbox;
	u32 mbox_slots[WLMGR_NUM_MAIL_SLOTS];
	mailboxPrepare(&mbox, mbox_slots, WLMGR_NUM_MAIL_SLOTS);

	// Set up PXI channels
	pxiSetHandler(PxiChannel_NetBuf, _wlmgrRxPxiHandler, &mbox);
	pxiSetMailbox(PxiChannel_WlMgr, &mbox);

	for (;;) {
		u32 msg = mailboxRecv(&mbox);
		if (msg != UINT32_MAX) {
			// Parse message
			WlMgrEvent evt = pxiWlMgrEventGetType(msg);
			unsigned imm = pxiWlMgrEventGetImm(msg);

			// Process event
			uptr arg0 = 0, arg1 = 0;
			switch (evt) {
				default: break;
				case WlMgrEvent_NewState: {
					arg0 = imm;
					arg1 = s_wlmgrState.state;
					s_wlmgrState.state = (WlMgrState)imm;
					break;
				}

				case WlMgrEvent_ScanComplete: {
					arg0 = (uptr)s_wlmgrState.scan_buf;
					arg1 = imm;
					s_wlmgrState.scan_buf = NULL;
					break;
				}

				case WlMgrEvent_CmdFailed: {
					s_wlmgrState.cmd_fail = true;
					// fallthrough
				}

				case WlMgrEvent_Disconnected: {
					arg0 = imm;
					break;
				}
			}

			// Call user event callback
			if (s_wlmgrState.event_cb) {
				s_wlmgrState.event_cb(s_wlmgrState.event_user, evt, arg0, arg1);
			}
		}

		// Atomically borrow the packet list
		NetBuf* pPacket = netbufQueueRemoveAll(&s_wlmgrState.rx_queue);

		// Process incoming packets
		NetBuf* pNext;
		for (; pPacket; pPacket = pNext) {
			pNext = pPacket->link.next;
			_wlmgrRssiBufUpdate(pPacket->user[0]);
			if (s_wlmgrState.rx_cb) {
				s_wlmgrState.rx_cb(s_wlmgrState.rx_user, pPacket);
			} else {
				netbufFree(pPacket);
			}
		}
	}

	return 0;
}

static bool _wlmgrInitNetBuf(const WlMgrInitConfig* config)
{
	// Validate params
	size_t avail_sz = config->pktmem_sz;
	if (((u32)config->pktmem & (ARM_CACHE_LINE_SZ-1)) || avail_sz < WLMGR_MIN_PACKET_MEM_SZ) {
		return false;
	}

	// Initialize tx/rx packet counts
	u16 tx_counts[5] = { 16, 5, 5, 2, 1 };
	u16 rx_counts[5] = {  8, 8, 5, 2, 1 };
	avail_sz -= WLMGR_MIN_PACKET_MEM_SZ;

	// Distribute all remaining memory
	u32 pattern = config->pktmem_allocmap;
	while (avail_sz >= sizeof(NetBuf) + 0x80) {
		unsigned i;
		size_t max_packet_sz = avail_sz - sizeof(NetBuf);
		if (max_packet_sz >= 0x800) {
			i = 4;
		} else if (max_packet_sz >= 0x400) {
			i = 3;
		} else if (max_packet_sz >= 0x200) {
			i = 2;
		} else if (max_packet_sz >= 0x100) {
			i = 1;
		} else /* if (max_packet_sz >= 0x80) */ {
			i = 0;
		}

		if (pattern&1) {
			tx_counts[i] ++;
		} else {
			rx_counts[i] ++;
		}

		avail_sz -= sizeof(NetBuf) + (0x80<<i);
		pattern = (pattern>>1) | (pattern<<30); // ROR 1
	}

	_netbufPrvInitPools(config->pktmem, tx_counts, rx_counts);
	return true;
}

bool wlmgrInit(const WlMgrInitConfig* config, u8 thread_prio)
{
	static bool initted = false;
	if (initted) {
		return true;
	}

	// Initialize netbuf system
	if (!config || !_wlmgrInitNetBuf(config)) {
		return false;
	}

	// Bring up event/rx thread
	threadPrepare(&s_wlmgrThread, _wlmgrThreadMain, NULL, &s_wlmgrThreadStack[sizeof(s_wlmgrThreadStack)], thread_prio);
	threadAttachLocalStorage(&s_wlmgrThread, NULL);
	threadStart(&s_wlmgrThread);

	// Wait for ARM7 to be available
	pxiWaitRemote(PxiChannel_WlMgr);

	initted = true;
	return true;
}

void wlmgrSetEventHandler(WlMgrEventFn cb, void* user)
{
	IrqState st = irqLock();
	s_wlmgrState.event_cb = cb;
	s_wlmgrState.event_user = user;
	irqUnlock(st);
}

WlMgrState wlmgrGetState(void)
{
	return s_wlmgrState.state;
}

unsigned wlmgrGetRssi(void)
{
	return s_wlmgrState.state >= WlMgrState_Associating ? _wlmgrRssiBufDigest() : 0;
}

bool wlmgrLastCmdFailed(void)
{
	return s_wlmgrState.cmd_fail;
}

void wlmgrStart(WlMgrMode mode)
{
	s_wlmgrState.cmd_fail = false;
	pxiSend(PxiChannel_WlMgr, pxiWlMgrMakeCmd(PxiWlMgrCmd_Start, mode));
}

void wlmgrStop(void)
{
	s_wlmgrState.cmd_fail = false;
	pxiSend(PxiChannel_WlMgr, pxiWlMgrMakeCmd(PxiWlMgrCmd_Stop, 0));
}

void wlmgrStartScan(WlanBssDesc* out_table, WlanBssScanFilter const* filter)
{
	// Check output buffer alignment
	u32 buf_addr = (u32)out_table;
	if (buf_addr & (ARM_CACHE_LINE_SZ-1)) {
		return;
	}

	// Copy filter to output buffer
	memcpy((void*)buf_addr, filter, sizeof(*filter));
	armDCacheFlush((void*)buf_addr, WLAN_MAX_BSS_ENTRIES*sizeof(WlanBssDesc));

	// Send command
	s_wlmgrState.scan_buf = out_table;
	s_wlmgrState.cmd_fail = false;
	pxiSendWithData(PxiChannel_WlMgr, pxiWlMgrMakeCmd(PxiWlMgrCmd_StartScan, 0), &buf_addr, 1);
}

void wlmgrAssociate(WlanBssDesc const* bss, WlanAuthData const* auth)
{
	_wlmgrRssiBufInit(bss->rssi);
	armDCacheFlush((void*)bss, sizeof(*bss));
	armDCacheFlush((void*)auth, sizeof(*auth));

	PxiWlMgrArgAssociate arg = {
		.bss  = bss,
		.auth = auth,
	};

	s_wlmgrState.cmd_fail = false;
	pxiSendWithData(PxiChannel_WlMgr, pxiWlMgrMakeCmd(PxiWlMgrCmd_Associate, 0), (const u32*)&arg, sizeof(arg)/sizeof(u32));
}

void wlmgrDisassociate(void)
{
	s_wlmgrState.cmd_fail = false;
	pxiSend(PxiChannel_WlMgr, pxiWlMgrMakeCmd(PxiWlMgrCmd_Disassociate, 0));
}

void wlmgrSetRawRxHandler(WlMgrRawRxFn cb, void* user)
{
	IrqState st = irqLock();
	s_wlmgrState.rx_cb = cb;
	s_wlmgrState.rx_user = user;
	irqUnlock(st);
}

void wlmgrRawTx(NetBuf* pPacket)
{
	uptr addr = (uptr)pPacket - MM_MAINRAM;
	netbufFlush(pPacket);
	pxiSend(PxiChannel_NetBuf, addr >> 5);
}
