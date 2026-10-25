/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* build the screens (call with the display lock held); the play screen is shown first */
void ui_build(int preset);
void ui_show_play(void);
/* the instrument screen (instrument.c) */
void instrument_build(void);
void instrument_show(void);
