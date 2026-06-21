// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/system/irq.h>
#include <calico/system/thread.h>
#include <calico/nds/system.h>
#include <calico/nds/env.h>
#include <calico/nds/lcd.h>
#include <calico/nds/touch.h>
#include <calico/nds/arm7/tsc.h>
#include <calico/nds/arm7/codec.h>
#include "../transfer.h"

// Touch stability filter params
#define TOUCH_MIN_THRESHOLD 20
#define TOUCH_MAX_THRESHOLD 35
#define TOUCH_HYSTERESIS 4

typedef struct TouchState {
	// Precalculated calibration data
	s32 xscale, yscale;
	s32 xoffset, yoffset;

	// Latched touch position
	TscTouchData latch_data;
	bool has_latch;

	// Touch stability filter
	u8  cur_threshold;
	u16 num_valid;
	u16 num_noisy;
} TouchState;

static TouchState s_touchState;

static Thread s_touchSrvThread;
alignas(8) static u8 s_touchSrvThreadStack[0x100];

static void _touchCalcPos(TouchData* data)
{
	int px = (data->rawx*s_touchState.xscale - s_touchState.xoffset + s_touchState.xscale/2)>>19;
	int py = (data->rawy*s_touchState.yscale - s_touchState.yoffset + s_touchState.yscale/2)>>19;

	if (px < 0) px = 0;
	else if (px > LCD_WIDTH-1) px = LCD_WIDTH-1;
	if (py < 0) py = 0;
	else if (py > LCD_HEIGHT-1) py = LCD_HEIGHT-1;

	data->px = px;
	data->py = py;
}

static int _touchSrvThreadMain(void* arg)
{
	// Configure VCount interrupt
	lcdSetVCountCompare(true, (unsigned)arg);
	irqEnable(IRQ_VCOUNT);

	for (;;) {
		threadIrqWait(false, IRQ_VCOUNT);

		TouchData data;
		bool valid = touchRead(&data);
		u16 state = s_transferRegion->touch_state ^ TOUCH_BUF;

		if (valid) {
			state |= TOUCH_VALID;

			// Copy data into transfer region
			TouchData* out = (TouchData*)s_transferRegion->touch_data[state & TOUCH_BUF];
			*out = data;
		} else {
			state &= ~TOUCH_VALID;
		}

		// Update state in transfer region
		s_transferRegion->touch_state = state;
	}

	return 0;
}

void touchInit(void)
{
	touchLoadCalibration();

	s_touchState.cur_threshold = TOUCH_MIN_THRESHOLD;

	spiLock();
	if (cdcIsTwlMode()) {
		cdcTscInit();
	} else {
		tscInit();
	}
	spiUnlock();
}

void touchStartServer(unsigned lyc, u8 thread_prio)
{
	threadPrepare(&s_touchSrvThread, _touchSrvThreadMain, (void*)lyc, &s_touchSrvThreadStack[sizeof(s_touchSrvThreadStack)], thread_prio);
	threadStart(&s_touchSrvThread);
}

void touchLoadCalibration(void)
{
#define CALIB g_envUserSettings->touch_calib
	s_touchState.xscale = ((CALIB.lcd_x2 - CALIB.lcd_x1) << 19) / (CALIB.raw_x2 - CALIB.raw_x1);
	s_touchState.yscale = ((CALIB.lcd_y2 - CALIB.lcd_y1) << 19) / (CALIB.raw_y2 - CALIB.raw_y1);
	s_touchState.xoffset = ((CALIB.raw_x1 + CALIB.raw_x2) * s_touchState.xscale - ((CALIB.lcd_x1 + CALIB.lcd_x2) << 19)) / 2;
	s_touchState.yoffset = ((CALIB.raw_y1 + CALIB.raw_y2) * s_touchState.yscale - ((CALIB.lcd_y1 + CALIB.lcd_y2) << 19)) / 2;
#undef CALIB
}

static void _touchUpdateFilter(TscResult res, u16 max_diff)
{
	switch (res) {
		// Touch not detected
		case TscResult_None: default: {
			s_touchState.num_valid = 0;
			s_touchState.num_noisy = 0;
			break;
		}

		// Touch detected, but data was noisy
		case TscResult_Noisy: {
			s_touchState.num_valid = 0;
			s_touchState.num_noisy++;
			if (s_touchState.num_noisy >= TOUCH_HYSTERESIS) {
				s_touchState.num_noisy = 0;
				if (s_touchState.cur_threshold < TOUCH_MAX_THRESHOLD) {
					s_touchState.cur_threshold++;
				}
			}
			break;
		}

		// Touch detected with valid data
		case TscResult_Valid: {
			s_touchState.num_noisy = 0;
			if (max_diff >= s_touchState.cur_threshold/2) {
				s_touchState.num_valid = 0;
			} else {
				s_touchState.num_valid++;
				if (s_touchState.num_valid >= TOUCH_HYSTERESIS) {
					s_touchState.num_valid = 0;
					if (s_touchState.cur_threshold > TOUCH_MIN_THRESHOLD) {
						s_touchState.cur_threshold--;
						s_touchState.num_noisy = TOUCH_HYSTERESIS-1;
					}
				}
			}
			break;
		}
	}
}

bool touchRead(TouchData* out)
{
	TscResult res;
	TscTouchData data;
	u16 max_diff;

	// Read touch data from TSC
	spiLock();
	if (cdcIsTwlMode()) {
		res = cdcTscReadTouch(&data, s_touchState.cur_threshold, &max_diff);
	} else {
		res = tscReadTouch(&data, s_touchState.cur_threshold, &max_diff);
	}
	spiUnlock();

	// Update touch stability filter
	_touchUpdateFilter(res, max_diff);

	// Return early if no touch is detected
	if (res == TscResult_None) {
		s_touchState.has_latch = false;
		return false;
	}

	// Check if valid touch data has been successfully read
	if_likely (res == TscResult_Valid) {
		// Latch this data in case we subsequently read noisy data
		s_touchState.has_latch = true;
		s_touchState.latch_data = data;
	} else /* if (res == TscResult_Noisy) */ {
		// Return early if we have no latched data
		if (!s_touchState.has_latch) {
			return false;
		}

		// Replace the noisy data with the latched data
		data = s_touchState.latch_data;
	}

	// Fill out return data
	out->rawx = data.x;
	out->rawy = data.y;
	_touchCalcPos(out);

	return true;
}
