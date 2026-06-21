// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/mutex.h>
#include <calico/system/tick.h>
#include <calico/nds/system.h>
#include <calico/nds/bios.h>
#include <calico/nds/arm7/gpio.h>
#include <calico/nds/arm7/rtc.h>
#include "../transfer.h"

#define RTC_CMD(_reg,_isread) (0x60 | (((_reg)&0xf)<<1) | ((_isread)&1))

static Mutex s_rtcMutex;
static TickTask s_rtcUpdateTask;

MK_INLINE void _rtcSleepUsec(int usec)
{
	// This is probably eyeballed (not by me) and a lot slower than what's necessary.
	// TODO: replace with a more accurate (and inline-asm) cycle count busyloop.
	svcWaitByLoop(usec<<3);
}

MK_INLINE void _rtcBegin(void)
{
	mutexLock(&s_rtcMutex);
	REG_RCNT_RTC = RCNT_RTC_CS_0 | RCNT_RTC_SCK_1;
	_rtcSleepUsec(1);
	REG_RCNT_RTC = RCNT_RTC_CS_1 | RCNT_RTC_SCK_1;
	_rtcSleepUsec(1);
}

MK_INLINE void _rtcEnd(void)
{
	REG_RCNT_RTC = RCNT_RTC_CS_0 | RCNT_RTC_SCK_1;
	mutexUnlock(&s_rtcMutex);
}

MK_INLINE void _rtcOutputBit(u8 bit)
{
	bit &= 1;
	REG_RCNT_RTC = RCNT_RTC_CS_1 | RCNT_RTC_SCK_0 | RCNT_RTC_SIO_OUT | bit;
	_rtcSleepUsec(5);
	REG_RCNT_RTC = RCNT_RTC_CS_1 | RCNT_RTC_SCK_1 | RCNT_RTC_SIO_OUT | bit;
	_rtcSleepUsec(5);
}

MK_INLINE u8 _rtcInputBit(void)
{
	REG_RCNT_RTC = RCNT_RTC_CS_1 | RCNT_RTC_SCK_0;
	_rtcSleepUsec(5);
	REG_RCNT_RTC = RCNT_RTC_CS_1 | RCNT_RTC_SCK_1;
	_rtcSleepUsec(5);
	return REG_RCNT_RTC & RCNT_RTC_SIO;
}

MK_INLINE void _rtcOutputCmdByte(u8 byte)
{
	// MSB to LSB
	for (int i = 7; i >= 0; i --) {
		_rtcOutputBit(byte>>i);
	}
}

MK_INLINE void _rtcOutputDataByte(u8 byte)
{
	// LSB to MSB
	for (int i = 0; i < 8; i ++) {
		_rtcOutputBit(byte>>i);
	}
}

MK_INLINE u8 _rtcInputDataByte(void)
{
	// LSB to MSB
	u8 byte = 0;
	for (int i = 0; i < 8; i ++) {
		byte |= _rtcInputBit()<<i;
	}
	return byte;
}

void rtcInit(void)
{
	// Read current RTC status
	u8 status1 = rtcReadRegister8(RtcReg_Status1);
	u8 status2 = rtcReadRegister8(RtcReg_Status2);

	// Reset the RTC on first use/voltage drop/test mode
	if ((status1 & (RTC_STATUS1_BLD|RTC_STATUS1_POC)) || (status2 & RTC_STATUS2_TEST)) {
		rtcWriteRegister8(RtcReg_Status1, status1 | RTC_STATUS1_RESET);
		status1 = rtcReadRegister8(RtcReg_Status1);

		if (systemIsTwlMode()) {
			// Behold! Nintendo's amazing bodge, used to feed the Atheros hardware
			// (DSi Wifi) a steady 32768Hz clock through the RTC chip's FOUT signal.
			// If Atheros doesn't receive this clock, it reportedly fails to process
			// WMI messages (according to gbatek).
			rtcWriteRegister8(RtcReg_FoutHi, 0x80);
			rtcWriteRegister8(RtcReg_FoutLo, 0x00);
		}
	}

	// Ensure 24-hour mode is selected
	if (!(status1 & RTC_STATUS1_24HRS)) {
		rtcWriteRegister8(RtcReg_Status1, status1 | RTC_STATUS1_24HRS);
	}

	// Disable the RTC interrupts
	rtcWriteRegister8(RtcReg_Status2, 0);
}

static void _rtcUpdateTask(TickTask* t)
{
	s_transferRegion->unix_time++;
}

void rtcSyncTime(void)
{
	tickTaskStop(&s_rtcUpdateTask);
	s_transferRegion->unix_time = rtcReadUnixTime();
	unsigned ticks = ticksFromHz(1);
	tickTaskStart(&s_rtcUpdateTask, _rtcUpdateTask, ticks/2, ticks); // /2 in order to fairly distribute the error
}

void rtcReadRegister(RtcRegister reg, void* data, size_t size)
{
	u8* data8 = (u8*)data;

	_rtcBegin();
	_rtcOutputCmdByte(RTC_CMD(reg,1));

	for (size_t i = 0; i < size; i ++) {
		data8[i] = _rtcInputDataByte();
	}

	_rtcEnd();
}

void rtcWriteRegister(RtcRegister reg, const void* data, size_t size)
{
	const u8* data8 = (const u8*)data;

	_rtcBegin();
	_rtcOutputCmdByte(RTC_CMD(reg,0));

	for (size_t i = 0; i < size; i ++) {
		_rtcOutputDataByte(data8[i]);
	}

	_rtcEnd();
}

void rtcReadDateTime(RtcDateTime* t)
{
	rtcReadRegister(RtcReg_DateTime, t, sizeof(*t));

	// Remove extraneous AM/PM flag
	t->hour &= 0x3f;

	// Decode all BCD fields
	u8* raw = (u8*)t;
	for (size_t i = 0; i < sizeof(*t); i ++) {
		raw[i] = rtcDecodeBcd(raw[i]);
	}
}

u32 rtcDateTimeToUnix(const RtcDateTime* t)
{
	// Table of month -> day offsets within a year
	static const u16 s_monthDayOffsets[] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
	};

	// Unpack/adjust values from RTC
	unsigned YY,MM,DD,hh,mm,ss;
	YY = 2000 + t->year;
	MM = t->month - 1;
	DD = t->day - 1;
	hh = t->hour;
	mm = t->minute;
	ss = t->second;

	// Calculate second index into the day
	unsigned seconds = ss + 60U*(mm + 60U*hh);

	// Classic first grade programming job
	// The correct algorithm is as follows:
	//bool isLeap = (YY % 4) == 0 && !((YY % 100) == 0 && !((YY % 400) == 0));
	// However, we only consider years 1970..2099. During this interval, none
	// of the 100-year exceptions apply (in fact, the 400-year exception cancels
	// the 100-year exception for the year 2000), so the following is correct:
	bool isLeap = (YY % 4) == 0;

	// Calculate day index into the year (adjusting for Feb 29)
	unsigned days = DD + s_monthDayOffsets[MM] + (isLeap && MM > 1 ? 1 : 0);

	// Add year offset (including extra days for leap years)
	// Using a count of leap years since 1968 (which was leap)
	// Not including 100-year exceptions (see above comment)
	YY -= 1970; days += 365U*YY + (YY+1)/4U;

	// Final calculation
	return seconds + 86400U*days;
}
