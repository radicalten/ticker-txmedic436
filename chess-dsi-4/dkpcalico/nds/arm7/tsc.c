// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/system/thread.h>
#include <calico/nds/arm7/gpio.h>
#include <calico/nds/arm7/spi.h>
#include <calico/nds/arm7/tsc.h>

MK_INLINE void _tscSpiBegin(u8 cmd)
{
	spiRawStartHold(SpiDev_TSC, SpiBaud_2MHz);
	spiRawWriteByte(cmd);
}

MK_INLINE void _tscSpiPreEnd(void)
{
	spiRawEndHold(SpiDev_TSC, SpiBaud_2MHz);
}

MK_INLINE bool _tscIsPenIrqAsserted(void)
{
	return ~REG_RCNT_EXT & RCNT_EXT_PENIRQ;
}

static void _tscPowerDownBetweenConversions(void)
{
	_tscSpiBegin(tscMakeCmd(TscChannel_T0, TscConvMode_12bit, TscPowerMode_Auto));
	spiRawWriteByte(0);
	_tscSpiPreEnd();
	spiRawWriteByte(0);
}

static TscResult _tscDetectTouch(bool prev_touch)
{
	// Ensure the TSC can send the PENIRQ signal
	_tscPowerDownBetweenConversions();

	// If PENIRQ is asserted, succeed early
	if (_tscIsPenIrqAsserted()) {
		return TscResult_Valid;
	}

	// Otherwise - if the pen was previously detected,
	// try checking it again in case the signal is noisy
	if_likely (prev_touch) {
		_tscPowerDownBetweenConversions();
		if (_tscIsPenIrqAsserted()) {
			// Pen is detected again, meaning we have a noisy signal.
			return TscResult_Noisy;
		}
	}

	// Pen is certainly not touching the screen
	return TscResult_None;
}

void tscInit(void)
{
	// Initialize TSC power mode (also disables the VREF signal)
	_tscPowerDownBetweenConversions();
}

static TscResult _tscTryReadChannel(u16* out, unsigned diff_threshold, TscChannel ch, u16* out_max_diff)
{
	u16 array[5];
	unsigned cmd = tscMakeCmd(ch, TscConvMode_12bit, TscPowerMode_AdcOn);

	// Read 5 samples from the channel
	_tscSpiBegin(cmd);
	for (unsigned i = 0; i < 5; i ++) {
		unsigned hi = spiRawReadByte() & 0x7f;
		unsigned lo = spiRawWriteReadByte(cmd);
		array[4-i] = (lo>>3) | (hi<<5);
	}

	// XX: We are in fact reading a *sixth* sample, and throwing the result away.
	_tscSpiPreEnd();
	spiRawWriteByte(0);

	// Calc max difference if requested
	if (out_max_diff) {
		unsigned max_diff = 0;
		for (unsigned i = 0; i < 5-1; i ++) {
			for (unsigned j = i+1; j < 5; j ++) {
				unsigned diff = tscAbs(array[i] - array[j]);
				if (diff > max_diff) {
					max_diff = diff;
				}
			}
		}

		*out_max_diff = max_diff;
	}

#if MK_NDS_TSC_USE_ALTERNATE_ALGORITHM
	// Select one among the 5 samples to use as reference point.
	// At least two more samples must be below a given threshold
	// to be considered as a valid (non-noisy) result.
	unsigned sum;
	unsigned num_valid;
	TscResult res = TscResult_Noisy;
	for (unsigned i = 0; res == TscResult_Noisy && i < 4; i ++) {
		sum = array[i];
		num_valid = 1;

		for (unsigned j = 0; j < 5; j ++) {
			if (i == j) continue;

			unsigned diff = tscAbs(array[i] - array[j]);
			if (diff < diff_threshold) {
				sum += array[j];
				num_valid++;
			}
		}

		res = num_valid >= (2+1) ? TscResult_Valid : TscResult_Noisy;
	}

	// The final result is the average of the selected samples
	*out = sum / num_valid;
	return res;
#else
	// Find the first combination of 3 samples that are below a
	// given threshold.
	for (unsigned i = 0; i < 5-2; i ++) {
		for (unsigned j = i+1; j < 5-1; j ++) {
			unsigned diff_j = tscAbs(array[i] - array[j]);
			if (diff_j >= diff_threshold) {
				continue;
			}

			for (unsigned k = j+1; k < 5; k ++) {
				unsigned diff_k = tscAbs(array[i] - array[k]);
				if (diff_k < diff_threshold) {
					// We found the points - now average them
					// XX: This uses an approximation to avoid the divide by 3
					*out = (2*array[i] + array[j] + array[k]) / 4;
					return TscResult_Valid;
				}
			}
		}
	}

	// If above didn't return, we have a noisy result.
	// XX: using the average of the first and last samples
	*out = (array[0] + array[4]) / 2;
	return TscResult_Noisy;
#endif
}

TscResult tscReadTouch(TscTouchData* out, unsigned diff_threshold, u16* out_max_diff)
{
	static bool touching = false;

	// Detect if the pen is touching the screen, and fail early if it's not
	TscResult res = _tscDetectTouch(touching);
	if (res == TscResult_None) {
		touching = false;
		return TscResult_None;
	}

	// Try to read X/Y coordinates
	u16 max_diff_x, max_diff_y;
	TscResult res_x = _tscTryReadChannel(&out->x, diff_threshold, TscChannel_X, &max_diff_x);
	TscResult res_y = _tscTryReadChannel(&out->y, diff_threshold, TscChannel_Y, &max_diff_y);

	// XX: 13 dummy zero bytes written to SPI here.
	// This does not seem necessary - tested on DS Phat (and DSi in NTR mode).

	// Run the touch detection again, in order to make sure the pen is down
	if (_tscDetectTouch(touching) != TscResult_Valid) {
		// Pen-up, or noisy touch twice in a row -> No longer touching
		touching = false;
		return TscResult_None;
	}

	// At this point we are confident the pen is touching the screen.
	touching = true;

	// Propagate noisy touch status
	if (res != TscResult_Noisy) {
		if (res_x == TscResult_Noisy || res_y == TscResult_Noisy) {
			res = TscResult_Noisy;
		}
	}

	// Propagate max difference if needed
	if (out_max_diff) {
		*out_max_diff = max_diff_x > max_diff_y ? max_diff_x : max_diff_y;
	}

	return res;
}

MK_CODE32 unsigned tscReadChannel8(TscChannel ch)
{
	_tscSpiBegin(tscMakeCmd(ch, TscConvMode_8bit, TscPowerMode_Auto));
	unsigned hi = spiRawReadByte() & 0x7f;
	_tscSpiPreEnd();
	unsigned lo = spiRawReadByte();

	return (hi << 1) | (lo >> 7);
}

MK_CODE32 unsigned tscReadChannel12(TscChannel ch)
{
	_tscSpiBegin(tscMakeCmd(ch, TscConvMode_12bit, TscPowerMode_Auto));
	unsigned hi = spiRawReadByte() & 0x7f;
	_tscSpiPreEnd();
	unsigned lo = spiRawReadByte();

	return (hi << 5) | (lo >> 3);
}
