#ifndef UAE_STATUSLINE_H
#define UAE_STATUSLINE_H

#include "uae/types.h"

#define TD_PADX 0
#define TD_PADY 2
#define TD_WIDTH 28
#define TD_LED_WIDTH 24
#define TD_LED_HEIGHT 4

#define TD_RIGHT 1
#define TD_BOTTOM 2

static int td_pos = (TD_RIGHT|TD_BOTTOM);

#define TD_NUM_WIDTH 7
#define TD_NUM_HEIGHT 7

#define TD_TOTAL_HEIGHT (TD_PADY * 2 + TD_NUM_HEIGHT)

#define NUMBERS_NUM 19

#define TD_BORDER 0x333

#define STATUSLINE_CHIPSET 1
#define STATUSLINE_RTG 2
#define STATUSLINE_TARGET 0x80

extern void draw_status_line_single (uae_u8 *buf, int y, int totalwidth);

#endif /* UAE_STATUSLINE_H */
