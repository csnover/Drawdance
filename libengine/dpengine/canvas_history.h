/*
 * Copyright (C) 2022 askmeaboutloom
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --------------------------------------------------------------------
 *
 * This code is based on Drawpile, using it under the GNU General Public
 * License, version 3. See 3rdparty/licenses/drawpile/COPYING for details.
 */
#ifndef DPENGINE_CANVAS_HISTORY_H
#define DPENGINE_CANVAS_HISTORY_H
#include "canvas_state.h"
#include <dpcommon/common.h>

typedef struct DP_DrawContext DP_DrawContext;
typedef struct DP_Message DP_Message;


#define DP_USER_CURSOR_COUNT 256

typedef struct DP_CanvasHistory DP_CanvasHistory;

typedef struct DP_UserCursorBuffer {
    int count;
    DP_UserCursor cursors[DP_USER_CURSOR_COUNT];
} DP_UserCursorBuffer;

typedef void (*DP_CanvasHistorySavePointFn)(void *user, DP_CanvasState *cs,
                                            bool snapshot_requested);

DP_CanvasHistory *
DP_canvas_history_new(DP_CanvasHistorySavePointFn save_point_fn,
                      void *save_point_user);

void DP_canvas_history_free(DP_CanvasHistory *ch);

void DP_canvas_history_local_drawing_in_progress_set(
    DP_CanvasHistory *ch, bool local_drawing_in_progress);

DP_CanvasState *
DP_canvas_history_compare_and_get(DP_CanvasHistory *ch, DP_CanvasState *prev,
                                  DP_UserCursorBuffer *out_user_cursors);

void DP_canvas_history_reset(DP_CanvasHistory *ch);

void DP_canvas_history_soft_reset(DP_CanvasHistory *ch);

bool DP_canvas_history_snapshot(DP_CanvasHistory *ch);

bool DP_canvas_history_handle(DP_CanvasHistory *ch, DP_DrawContext *dc,
                              DP_Message *msg);

bool DP_canvas_history_handle_local(DP_CanvasHistory *ch, DP_DrawContext *dc,
                                    DP_Message *msg);

void DP_canvas_history_handle_multidab_dec(DP_CanvasHistory *ch,
                                           DP_DrawContext *dc, int count,
                                           DP_Message **msgs);

void DP_canvas_history_handle_local_multidab_dec(DP_CanvasHistory *ch,
                                                 DP_DrawContext *dc, int count,
                                                 DP_Message **msgs);


#endif
