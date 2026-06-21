// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/nds/touch.h>
#include "../transfer.h"

bool touchRead(TouchData* out)
{
	u16 state, state_old;
	bool valid;

	state = s_transferRegion->touch_state;
	do {
		valid = state & TOUCH_VALID;
		if (valid) {
			TouchData* data = (TouchData*)s_transferRegion->touch_data[state & TOUCH_BUF];
			*out = *data;
		}

		state_old = state;
		state = s_transferRegion->touch_state;
	} while (state != state_old);

	if (!valid) {
		*out = (TouchData){0};
	}

	return valid;
}
