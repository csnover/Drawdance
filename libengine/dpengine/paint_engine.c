#include "paint_engine.h"
#include "canvas_diff.h"
#include "canvas_history.h"
#include "canvas_state.h"
#include "draw_context.h"
#include "layer_content.h"
#include "layer_group.h"
#include "layer_list.h"
#include "layer_props.h"
#include "layer_props_list.h"
#include "layer_routes.h"
#include "paint.h"
#include "tile.h"
#include <dpcommon/atomic.h>
#include <dpcommon/common.h>
#include <dpcommon/conversions.h>
#include <dpcommon/queue.h>
#include <dpcommon/threading.h>
#include <dpcommon/worker.h>
#include <dpmsg/acl.h>
#include <dpmsg/blend_mode.h>
#include <dpmsg/message.h>
#include <dpmsg/message_queue.h>
#include <dpmsg/msg_internal.h>
#include <limits.h>


#define INITIAL_QUEUE_CAPACITY 64

#define PREVIEW_SUBLAYER_ID -100
#define INSPECT_SUBLAYER_ID -200

typedef struct DP_PaintEnginePreview DP_PaintEnginePreview;
typedef DP_CanvasState *(*DP_PaintEnginePreviewRenderFn)(
    DP_PaintEnginePreview *preview, DP_CanvasState *cs, DP_DrawContext *dc,
    int offset_x, int offset_y);
typedef void (*DP_PaintEnginePreviewDisposeFn)(DP_PaintEnginePreview *preview);

struct DP_PaintEnginePreview {
    int initial_offset_x, initial_offset_y;
    DP_PaintEnginePreviewRenderFn render;
    DP_PaintEnginePreviewDisposeFn dispose;
};

static DP_PaintEnginePreview null_preview;

typedef struct DP_PaintEngineCutPreview {
    DP_PaintEnginePreview parent;
    DP_LayerContent *lc;
    DP_LayerProps *lp;
    int layer_id;
    int x, y;
    int width, height;
    bool have_mask;
    uint8_t mask[];
} DP_PaintEngineCutPreview;

typedef struct DP_PaintEngineDabsPreview {
    DP_PaintEnginePreview parent;
    int layer_id;
    int count;
    DP_Message *messages[];
} DP_PaintEngineDabsPreview;

typedef struct DP_PaintEngineLaserBuffer {
    uint8_t persistence;
    uint8_t b, g, r, a;
} DP_PaintEngineLaserBuffer;

typedef struct DP_PaintEngineLaserChanges {
    uint8_t count;
    uint8_t users[256];
    bool active[256];
    DP_PaintEngineLaserBuffer buffers[256];
} DP_PaintEngineLaserChanges;

typedef struct DP_PaintEngineCursorPosition {
    int32_t x, y;
} DP_PaintEngineCursorPosition;

typedef struct DP_PaintEngineCursorChanges {
    uint8_t count;
    uint8_t users[256];
    bool active[256];
    DP_PaintEngineCursorPosition positions[256];
} DP_PaintEngineCursorChanges;

typedef struct DP_PaintEngineMetaBuffer {
    uint8_t acl_change_flags;
    bool have_default_layer;
    uint16_t default_layer;
    DP_PaintEngineLaserChanges laser_changes;
    DP_PaintEngineCursorChanges cursor_changes;
} DP_PaintEngineMetaBuffer;

struct DP_PaintEngine {
    DP_AclState *acls;
    DP_CanvasHistory *ch;
    DP_CanvasDiff *diff;
    DP_TransientLayerContent *tlc;
    DP_Tile *checker;
    DP_CanvasState *history_cs;
    DP_CanvasState *view_cs;
    struct {
        int active_layer_id;
        int active_frame_index;
        DP_LayerViewMode layer_view_mode;
        bool reveal_censored;
        unsigned int inspect_context_id;
        struct {
            int used;
            int capacity;
            int *layer_ids;
        } hidden_layers;
        DP_LayerPropsList *prev_lpl;
        DP_LayerPropsList *lpl;
    } local_view;
    DP_DrawContext *paint_dc;
    DP_PaintEnginePreview *preview;
    DP_DrawContext *preview_dc;
    DP_Queue local_queue;
    DP_Queue remote_queue;
    DP_Semaphore *queue_sem;
    DP_Mutex *queue_mutex;
    DP_Atomic running;
    DP_Atomic catchup;
    DP_AtomicPtr next_preview;
    DP_Thread *paint_thread;
    struct {
        DP_Worker *worker;
        DP_Semaphore *tiles_done_sem;
        int tiles_waiting;
    } render;
    alignas(max_align_t) unsigned char buffers[];
};

struct DP_PaintEngineRenderParams {
    DP_PaintEngine *pe;
    int xtiles;
    DP_PaintEngineRenderTileFn render_tile;
    void *user;
};

struct DP_PaintEngineRenderJobParams {
    struct DP_PaintEngineRenderParams *render_params;
    int x, y;
};


static void free_preview(DP_PaintEnginePreview *preview)
{
    if (preview && preview != &null_preview) {
        preview->dispose(preview);
        DP_free(preview);
    }
}

static void handle_internal(DP_PaintEngine *pe, DP_DrawContext *dc,
                            DP_MsgInternal *mi)
{
    DP_MsgInternalType type = DP_msg_internal_type(mi);
    switch (type) {
    case DP_MSG_INTERNAL_TYPE_RESET:
        DP_canvas_history_reset(pe->ch);
        break;
    case DP_MSG_INTERNAL_TYPE_SOFT_RESET:
        DP_canvas_history_soft_reset(pe->ch);
        break;
    case DP_MSG_INTERNAL_TYPE_SNAPSHOT:
        if (!DP_canvas_history_snapshot(pe->ch)) {
            DP_warn("Error requesting snapshot: %s", DP_error());
        }
        break;
    case DP_MSG_INTERNAL_TYPE_CATCHUP:
        DP_atomic_set(&pe->catchup, DP_msg_internal_catchup_progress(mi));
        break;
    case DP_MSG_INTERNAL_TYPE_CLEANUP:
        DP_canvas_history_cleanup(pe->ch, dc);
        break;
    case DP_MSG_INTERNAL_TYPE_PREVIEW:
        free_preview(DP_atomic_ptr_xch(&pe->next_preview,
                                       DP_msg_internal_preview_data(mi)));
        break;
    default:
        DP_warn("Unhandled internal message type %d", (int)type);
        break;
    }
}


// Since draw dabs messages are so common and come in bunches, we have special
// handling to deal with them in batches. That makes the code more complicated,
// but it gives significantly better performance, so it's worth it in the end.

// These limits are static and arbitrary. They could surely be improved
// by making them dynamic or at least measuring what good limits are.

// Maximum number of multidab messages in a single go.
#define MAX_MULTIDAB_MESSAGES 1024
// Largest area that all dabs together are allowed to cover.
#define MAX_MULTIDAB_AREA (256 * 256 * 16)
// Threshold that the first message must not exceed to keep shifting more
// messages. Presumably if the first message reaches half of our limit, the next
// message is likely to push us over it, so we don't even need to try.
#define MAX_MULTIDAB_AREA_THRESHOLD (MAX_MULTIDAB_AREA / 2)

static bool shift_first_message(DP_PaintEngine *pe, DP_Message **msgs)
{
    // Local queue takes priority, we want our own strokes to be responsive.
    DP_Message *msg = DP_message_queue_shift(&pe->local_queue);
    if (msg) {
        msgs[0] = msg;
        return true;
    }
    else {
        msgs[0] = DP_message_queue_shift(&pe->remote_queue);
        return false;
    }
}

static int get_classic_dabs_area(DP_MsgDrawDabsClassic *mddc, int dabs_area)
{
    int count;
    const DP_ClassicDab *cds = DP_msg_draw_dabs_classic_dabs(mddc, &count);
    for (int i = 0; i < count && dabs_area < MAX_MULTIDAB_AREA; ++i) {
        int radius = DP_classic_dab_size(DP_classic_dab_at(cds, i)) / 256;
        int diameter = radius * 2;
        int area = DP_max_int(1, diameter * diameter);
        dabs_area += area;
    }
    return dabs_area;
}

static int get_pixel_dabs_area(DP_MsgDrawDabsPixel *mddp, int dabs_area)
{
    int count;
    const DP_PixelDab *pds = DP_msg_draw_dabs_pixel_dabs(mddp, &count);
    for (int i = 0; i < count && dabs_area < MAX_MULTIDAB_AREA; ++i) {
        int radius = DP_pixel_dab_size(DP_pixel_dab_at(pds, i));
        int diameter = radius * 2;
        int area = DP_max_int(1, diameter * diameter);
        dabs_area += area;
    }
    return dabs_area;
}

static int get_mypaint_dabs_area(DP_MsgDrawDabsMyPaint *mddmp, int dabs_area)
{
    int count;
    const DP_MyPaintDab *mpds = DP_msg_draw_dabs_mypaint_dabs(mddmp, &count);
    for (int i = 0; i < count && dabs_area < MAX_MULTIDAB_AREA; ++i) {
        // FIXME: size is supposed to be the radius, not the diameter. I think.
        // But currently, the painting code makes this mistake as well, so this
        // is the correct way of counting the dab size.
        int diameter = DP_mypaint_dab_size(DP_mypaint_dab_at(mpds, i)) / 256;
        int area = DP_max_int(1, diameter * diameter);
        dabs_area += area;
    }
    return dabs_area;
}

static int get_dabs_area(DP_Message *msg, DP_MessageType type, int dabs_area)
{
    switch (type) {
    case DP_MSG_DRAW_DABS_CLASSIC:
        return get_classic_dabs_area(DP_message_internal(msg), dabs_area);
    case DP_MSG_DRAW_DABS_PIXEL:
    case DP_MSG_DRAW_DABS_PIXEL_SQUARE:
        return get_pixel_dabs_area(DP_message_internal(msg), dabs_area);
    case DP_MSG_DRAW_DABS_MYPAINT:
        return get_mypaint_dabs_area(DP_message_internal(msg), dabs_area);
    default:
        return MAX_MULTIDAB_AREA + 1;
    }
}

static int shift_more_draw_dabs_messages(DP_PaintEngine *pe, bool local,
                                         DP_Message **msgs,
                                         int initial_dabs_area)
{
    int count = 1;
    int total_dabs_area = initial_dabs_area;
    DP_Queue *queue = local ? &pe->local_queue : &pe->remote_queue;

    DP_Message *msg;
    while (count < MAX_MULTIDAB_MESSAGES
           && (msg = DP_message_queue_peek(queue))) {
        total_dabs_area =
            get_dabs_area(msg, DP_message_type(msg), total_dabs_area);
        if (total_dabs_area <= MAX_MULTIDAB_AREA) {
            DP_queue_shift(queue);
            msgs[count++] = msg;
        }
        else {
            break;
        }
    }

    if (count > 1) {
        int n = count - 1;
        DP_ASSERT(DP_semaphore_value(pe->queue_sem) >= n);
        DP_SEMAPHORE_MUST_WAIT_N(pe->queue_sem, n);
    }

    return count;
}

static int maybe_shift_more_messages(DP_PaintEngine *pe, bool local,
                                     DP_MessageType type, DP_Message **msgs)
{
    int dabs_area = get_dabs_area(msgs[0], type, 0);
    if (dabs_area <= MAX_MULTIDAB_AREA_THRESHOLD) {
        return shift_more_draw_dabs_messages(pe, local, msgs, dabs_area);
    }
    else {
        return 1;
    }
}

static void handle_single_message(DP_PaintEngine *pe, DP_DrawContext *dc,
                                  bool local, DP_MessageType type,
                                  DP_Message *msg)
{
    if (type == DP_MSG_INTERNAL) {
        handle_internal(pe, dc, DP_msg_internal_cast(msg));
    }
    else if (local) {
        if (!DP_canvas_history_handle_local(pe->ch, dc, msg)) {
            DP_warn("Handle local command: %s", DP_error());
        }
    }
    else {
        if (!DP_canvas_history_handle(pe->ch, dc, msg)) {
            DP_warn("Handle remote command: %s", DP_error());
        }
    }
    DP_message_decref(msg);
}

static void handle_multidab(DP_PaintEngine *pe, DP_DrawContext *dc, bool local,
                            int count, DP_Message **msgs)
{
    if (local) {
        DP_canvas_history_handle_local_multidab_dec(pe->ch, dc, count, msgs);
    }
    else {
        DP_canvas_history_handle_multidab_dec(pe->ch, dc, count, msgs);
    }
}

static void handle_message(DP_PaintEngine *pe, DP_DrawContext *dc,
                           DP_Message **msgs)
{
    DP_MUTEX_MUST_LOCK(pe->queue_mutex);
    bool local = shift_first_message(pe, msgs);
    DP_Message *first = msgs[0];
    DP_MessageType type = DP_message_type(first);
    int count = maybe_shift_more_messages(pe, local, type, msgs);
    DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);

    DP_ASSERT(count > 0);
    DP_ASSERT(count <= MAX_MULTIDAB_MESSAGES);
    if (count == 1) {
        handle_single_message(pe, dc, local, type, first);
    }
    else {
        handle_multidab(pe, dc, local, count, msgs);
    }
}

static void run_paint_engine(void *user)
{
    DP_PaintEngine *pe = user;
    DP_DrawContext *dc = pe->paint_dc;
    DP_Message **msgs = DP_malloc(sizeof(*msgs) * MAX_MULTIDAB_MESSAGES);
    while (true) {
        DP_SEMAPHORE_MUST_WAIT(pe->queue_sem);
        if (DP_atomic_get(&pe->running)) {
            handle_message(pe, dc, msgs);
        }
        else {
            break;
        }
    }
    DP_free(msgs);
}


static void render_job(void *user, int thread_index)
{
    struct DP_PaintEngineRenderJobParams *job_params = user;
    struct DP_PaintEngineRenderParams *render_params =
        job_params->render_params;
    int x = job_params->x;
    int y = job_params->y;

    DP_PaintEngine *pe = render_params->pe;
    DP_TransientLayerContent *tlc = pe->tlc;
    DP_TransientTile *tt = DP_transient_layer_content_render_tile(
        tlc, pe->view_cs, y * render_params->xtiles + x);
    DP_transient_tile_merge(tt, pe->checker, DP_BIT15, DP_BLEND_MODE_BEHIND);

    DP_Tile *t = DP_transient_layer_content_tile_at_noinc(tlc, x, y);
    DP_Pixel8 *pixel_buffer =
        ((DP_Pixel8 *)pe->buffers) + DP_TILE_LENGTH * thread_index;
    DP_pixels15_to_8(pixel_buffer, DP_tile_pixels(t), DP_TILE_LENGTH);

    render_params->render_tile(render_params->user, x, y, pixel_buffer,
                               thread_index);

    DP_SEMAPHORE_MUST_POST(pe->render.tiles_done_sem);
}


DP_PaintEngine *
DP_paint_engine_new_inc(DP_DrawContext *paint_dc, DP_DrawContext *preview_dc,
                        DP_AclState *acls, DP_CanvasState *cs_or_null,
                        DP_CanvasHistorySavePointFn save_point_fn,
                        void *save_point_user)
{
    int render_thread_count = DP_thread_cpu_count();
    size_t flex_size = DP_max_size(sizeof(DP_PaintEngineLaserBuffer),
                                   sizeof(DP_Pixel8[DP_TILE_LENGTH])
                                       * DP_int_to_size(render_thread_count));
    DP_PaintEngine *pe =
        DP_malloc(DP_FLEX_SIZEOF(DP_PaintEngine, buffers, flex_size));

    pe->acls = acls;
    pe->ch =
        DP_canvas_history_new_inc(cs_or_null, save_point_fn, save_point_user);
    pe->diff = DP_canvas_diff_new();
    pe->tlc = DP_transient_layer_content_new_init(0, 0, NULL);
    pe->checker = DP_tile_new_checker(
        0, (DP_Pixel15){DP_BIT15 / 2, DP_BIT15 / 2, DP_BIT15 / 2, DP_BIT15},
        (DP_Pixel15){DP_BIT15, DP_BIT15, DP_BIT15, DP_BIT15});
    pe->history_cs = DP_canvas_state_new();
    pe->view_cs = DP_canvas_state_incref(pe->history_cs);
    pe->local_view.active_layer_id = 0;
    pe->local_view.active_frame_index = 0;
    pe->local_view.layer_view_mode = DP_LAYER_VIEW_MODE_NORMAL;
    pe->local_view.reveal_censored = false;
    pe->local_view.inspect_context_id = 0;
    pe->local_view.hidden_layers.used = 0;
    pe->local_view.hidden_layers.capacity = 0;
    pe->local_view.hidden_layers.layer_ids = NULL;
    pe->local_view.prev_lpl = NULL;
    pe->local_view.lpl = NULL;
    pe->paint_dc = paint_dc;
    pe->preview = NULL;
    pe->preview_dc = preview_dc;
    DP_message_queue_init(&pe->local_queue, INITIAL_QUEUE_CAPACITY);
    DP_message_queue_init(&pe->remote_queue, INITIAL_QUEUE_CAPACITY);
    pe->queue_sem = DP_semaphore_new(0);
    pe->queue_mutex = DP_mutex_new();
    DP_atomic_set(&pe->running, true);
    DP_atomic_set(&pe->catchup, -1);
    DP_atomic_ptr_set(&pe->next_preview, NULL);
    pe->paint_thread = DP_thread_new(run_paint_engine, pe);
    pe->render.worker =
        DP_worker_new(1024, sizeof(struct DP_PaintEngineRenderJobParams),
                      render_thread_count, render_job);
    pe->render.tiles_done_sem = DP_semaphore_new(0);
    pe->render.tiles_waiting = 0;
    return pe;
}

void DP_paint_engine_free_join(DP_PaintEngine *pe)
{
    if (pe) {
        DP_atomic_set(&pe->running, false);
        DP_semaphore_free(pe->render.tiles_done_sem);
        DP_worker_free_join(pe->render.worker);
        DP_SEMAPHORE_MUST_POST(pe->queue_sem);
        DP_thread_free_join(pe->paint_thread);
        DP_mutex_free(pe->queue_mutex);
        DP_semaphore_free(pe->queue_sem);
        free_preview(DP_atomic_ptr_xch(&pe->next_preview, NULL));
        DP_message_queue_dispose(&pe->remote_queue);
        DP_Message *msg;
        while ((msg = DP_message_queue_shift(&pe->local_queue))) {
            if (DP_message_type(msg) == DP_MSG_INTERNAL) {
                DP_MsgInternal *mi = DP_msg_internal_cast(msg);
                if (DP_msg_internal_type(mi) == DP_MSG_INTERNAL_TYPE_PREVIEW) {
                    free_preview(DP_msg_internal_preview_data(mi));
                }
            }
            DP_message_decref(msg);
        }
        DP_message_queue_dispose(&pe->local_queue);
        free_preview(pe->preview);
        DP_layer_props_list_decref_nullable(pe->local_view.lpl);
        DP_layer_props_list_decref_nullable(pe->local_view.prev_lpl);
        DP_free(pe->local_view.hidden_layers.layer_ids);
        DP_canvas_state_decref_nullable(pe->history_cs);
        DP_canvas_state_decref_nullable(pe->view_cs);
        DP_tile_decref(pe->checker);
        DP_transient_layer_content_decref(pe->tlc);
        DP_canvas_diff_free(pe->diff);
        DP_canvas_history_free(pe->ch);
        DP_free(pe);
    }
}

int DP_paint_engine_render_thread_count(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return DP_worker_thread_count(pe->render.worker);
}

DP_TransientLayerContent *
DP_paint_engine_render_content_noinc(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return pe->tlc;
}

void DP_paint_engine_local_drawing_in_progress_set(
    DP_PaintEngine *pe, bool local_drawing_in_progress)
{
    DP_ASSERT(pe);
    DP_canvas_history_local_drawing_in_progress_set(pe->ch,
                                                    local_drawing_in_progress);
}


static void invalidate_local_view(DP_PaintEngine *pe)
{
    DP_layer_props_list_decref_nullable(pe->local_view.prev_lpl);
    pe->local_view.prev_lpl = NULL;
}

void DP_paint_engine_active_layer_id_set(DP_PaintEngine *pe, int layer_id)
{
    if (pe->local_view.active_layer_id != layer_id) {
        pe->local_view.active_layer_id = layer_id;
        if (pe->local_view.layer_view_mode != DP_LAYER_VIEW_MODE_NORMAL) {
            invalidate_local_view(pe);
        }
    }
}

void DP_paint_engine_active_frame_index_set(DP_PaintEngine *pe, int frame_index)
{
    if (pe->local_view.active_frame_index != frame_index) {
        pe->local_view.active_frame_index = frame_index;
        DP_LayerViewMode mode = pe->local_view.layer_view_mode;
        if (mode == DP_LAYER_VIEW_MODE_FRAME
            || mode == DP_LAYER_VIEW_MODE_ONION_SKIN) {
            invalidate_local_view(pe);
        }
    }
}

void DP_paint_engine_view_mode_set(DP_PaintEngine *pe, DP_LayerViewMode mode)
{
    DP_ASSERT(pe);
    if (pe->local_view.layer_view_mode != mode) {
        pe->local_view.layer_view_mode = mode;
        invalidate_local_view(pe);
    }
}

bool DP_paint_engine_reveal_censored(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return pe->local_view.reveal_censored;
}

void DP_paint_engine_reveal_censored_set(DP_PaintEngine *pe,
                                         bool reveal_censored)
{
    DP_ASSERT(pe);
    if (pe->local_view.reveal_censored != reveal_censored) {
        pe->local_view.reveal_censored = reveal_censored;
        invalidate_local_view(pe);
    }
}


static int search_hidden_layer_index(DP_PaintEngine *pe, int layer_id)
{
    int used = pe->local_view.hidden_layers.used;
    int *layer_ids = pe->local_view.hidden_layers.layer_ids;
    for (int i = 0; i < used; ++i) {
        if (layer_ids[i] == layer_id) {
            return i;
        }
    }
    return -1;
}

static void insert_hidden_layer(DP_PaintEngine *pe, int layer_id)
{
    int used = pe->local_view.hidden_layers.used++;
    int capacity = pe->local_view.hidden_layers.capacity;
    int *layer_ids = pe->local_view.hidden_layers.layer_ids;
    if (used == capacity) {
        int new_capacity = DP_max_int(8, capacity * 2);
        layer_ids = DP_realloc(layer_ids, DP_int_to_size(new_capacity)
                                              * sizeof(*layer_ids));
        pe->local_view.hidden_layers.layer_ids = layer_ids;
        pe->local_view.hidden_layers.capacity = new_capacity;
    }
    layer_ids[used] = layer_id;
}

static void remove_hidden_layer(DP_PaintEngine *pe, int index)
{
    int *layer_ids = pe->local_view.hidden_layers.layer_ids;
    int last = --pe->local_view.hidden_layers.used;
    layer_ids[index] = layer_ids[last];
}

void DP_paint_engine_layer_visibility_set(DP_PaintEngine *pe, int layer_id,
                                          bool hidden)
{
    DP_ASSERT(pe);
    int index = search_hidden_layer_index(pe, layer_id);
    if (hidden && index == -1) {
        insert_hidden_layer(pe, layer_id);
        invalidate_local_view(pe);
    }
    else if (!hidden && index != -1) {
        remove_hidden_layer(pe, index);
        invalidate_local_view(pe);
    }
}

void DP_paint_engine_inspect_context_id_set(DP_PaintEngine *pe,
                                            unsigned int context_id)
{
    DP_ASSERT(pe);
    if (pe->local_view.inspect_context_id != context_id) {
        pe->local_view.inspect_context_id = context_id;
        invalidate_local_view(pe);
    }
}


static bool is_internal_or_command(DP_MessageType type)
{
    return type >= 128 || type == DP_MSG_INTERNAL;
}

static DP_PaintEngineMetaBuffer *get_meta_buffer(DP_PaintEngine *pe)
{
    return (DP_PaintEngineMetaBuffer *)pe->buffers;
}

static void handle_laser_trail(DP_PaintEngine *pe, DP_Message *msg)
{
    uint8_t context_id = DP_uint_to_uint8(DP_message_context_id(msg));
    DP_PaintEngineLaserChanges *laser = &get_meta_buffer(pe)->laser_changes;

    int laser_count = laser->count;
    if (laser_count == 0) {
        memset(laser->active, 0, sizeof(laser->active));
        laser->users[0] = context_id;
        laser->count = 1;
    }
    else if (!laser->active[context_id]) {
        laser->active[context_id] = true;
        laser->users[laser_count] = context_id;
        ++laser->count;
    }

    DP_MsgLaserTrail *mlt = DP_msg_laser_trail_cast(msg);
    DP_UPixel8 pixel = {DP_msg_laser_trail_color(mlt)};
    laser->buffers[context_id] =
        (DP_PaintEngineLaserBuffer){DP_msg_laser_trail_persistence(mlt),
                                    pixel.b, pixel.g, pixel.r, pixel.a};
}

static void handle_move_pointer(DP_PaintEngine *pe, DP_Message *msg)
{
    uint8_t context_id = DP_uint_to_uint8(DP_message_context_id(msg));
    DP_PaintEngineCursorChanges *cursor = &get_meta_buffer(pe)->cursor_changes;

    int cursor_count = cursor->count;
    if (cursor_count == 0) {
        memset(cursor->active, 0, sizeof(cursor->active));
        cursor->users[0] = context_id;
        cursor->count = 1;
    }
    else if (!cursor->active[context_id]) {
        cursor->active[context_id] = true;
        cursor->users[cursor_count] = context_id;
        ++cursor->count;
    }

    DP_MsgMovePointer *mmp = DP_msg_move_pointer_cast(msg);
    cursor->positions[context_id] = (DP_PaintEngineCursorPosition){
        DP_msg_move_pointer_x(mmp), DP_msg_move_pointer_y(mmp)};
}

static void handle_default_layer(DP_PaintEngine *pe, DP_Message *msg)
{
    DP_MsgDefaultLayer *mdl = DP_msg_default_layer_cast(msg);
    DP_PaintEngineMetaBuffer *meta_buffer = get_meta_buffer(pe);
    meta_buffer->have_default_layer = true;
    meta_buffer->default_layer = DP_msg_default_layer_id(mdl);
}

static bool should_push_message_remote(DP_PaintEngine *pe, DP_Message *msg)
{
    uint8_t result = DP_acl_state_handle(pe->acls, msg);
    get_meta_buffer(pe)->acl_change_flags |= result;
    if (!(result & DP_ACL_STATE_FILTERED_BIT)) {
        DP_MessageType type = DP_message_type(msg);
        if (is_internal_or_command(type)) {
            return true;
        }
        else if (type == DP_MSG_LASER_TRAIL) {
            handle_laser_trail(pe, msg);
        }
        else if (type == DP_MSG_MOVE_POINTER) {
            handle_move_pointer(pe, msg);
        }
        else if (type == DP_MSG_DEFAULT_LAYER) {
            handle_default_layer(pe, msg);
        }
    }
    return false;
}

static bool should_push_message_local(DP_UNUSED DP_PaintEngine *pe,
                                      DP_Message *msg)
{
    DP_MessageType type = DP_message_type(msg);
    return is_internal_or_command(type);
}

static int push_messages(DP_PaintEngine *pe, DP_Queue *queue, int count,
                         DP_Message **msgs,
                         bool (*should_push)(DP_PaintEngine *, DP_Message *))
{
    DP_MUTEX_MUST_LOCK(pe->queue_mutex);
    // First message is the one that triggered the call to this function,
    // push it unconditionally. Then keep checking the rest again.
    int pushed = 1;
    DP_message_queue_push_inc(queue, msgs[0]);
    for (int i = 1; i < count; ++i) {
        DP_Message *msg = msgs[i];
        if (should_push(pe, msg)) {
            DP_message_queue_push_inc(queue, msg);
            ++pushed;
        }
    }
    DP_SEMAPHORE_MUST_POST_N(pe->queue_sem, pushed);
    DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
    return pushed;
}

int DP_paint_engine_handle_inc(
    DP_PaintEngine *pe, bool local, int count, DP_Message **msgs,
    DP_PaintEngineAclsChangedFn acls_changed,
    DP_PaintEngineLaserTrailFn laser_trail,
    DP_PaintEngineMovePointerFn move_pointer,
    DP_PaintEngineDefaultLayerSetFn default_layer_set, void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(msgs);

    bool (*should_push)(DP_PaintEngine *, DP_Message *) =
        local ? should_push_message_local : should_push_message_remote;

    DP_PaintEngineMetaBuffer *meta_buffer = get_meta_buffer(pe);
    meta_buffer->acl_change_flags = 0;
    meta_buffer->laser_changes.count = 0;
    meta_buffer->cursor_changes.count = 0;
    meta_buffer->have_default_layer = false;

    // Don't lock anything until we actually find a message to push.
    int pushed = 0;
    for (int i = 0; i < count; ++i) {
        if (should_push(pe, msgs[i])) {
            pushed =
                push_messages(pe, local ? &pe->local_queue : &pe->remote_queue,
                              count - i, msgs + i, should_push);
            break;
        }
    }

    int acl_change_flags =
        meta_buffer->acl_change_flags & DP_ACL_STATE_CHANGE_MASK;
    if (acl_change_flags != 0) {
        acls_changed(user, acl_change_flags);
    }

    DP_PaintEngineLaserChanges *laser = &meta_buffer->laser_changes;
    int laser_count = laser->count;
    for (int i = 0; i < laser_count; ++i) {
        uint8_t context_id = laser->users[i];
        DP_PaintEngineLaserBuffer *lb = &laser->buffers[context_id];
        DP_UPixel8 pixel = {.b = lb->b, .g = lb->g, .r = lb->r, .a = lb->a};
        laser_trail(user, context_id, lb->persistence, pixel.color);
    }

    DP_PaintEngineCursorChanges *cursor = &meta_buffer->cursor_changes;
    int cursor_count = cursor->count;
    for (int i = 0; i < cursor_count; ++i) {
        uint8_t context_id = cursor->users[i];
        DP_PaintEngineCursorPosition *cp = &cursor->positions[context_id];
        move_pointer(user, context_id, DP_int32_to_int(cp->x),
                     DP_int32_to_int(cp->y));
    }

    if (meta_buffer->have_default_layer) {
        default_layer_set(user, meta_buffer->default_layer);
    }

    return pushed;
}


static DP_CanvasState *apply_preview(DP_PaintEngine *pe, DP_CanvasState *cs)
{
    DP_PaintEnginePreview *preview = pe->preview;
    if (preview) {
        return preview->render(
            preview, cs, pe->preview_dc,
            preview->initial_offset_x - DP_canvas_state_offset_x(cs),
            preview->initial_offset_y - DP_canvas_state_offset_y(cs));
    }
    else {
        return DP_canvas_state_incref(cs);
    }
}

static DP_TransientCanvasState *
get_or_make_transient_canvas_state(DP_CanvasState *cs)
{
    if (DP_canvas_state_transient(cs)) {
        return (DP_TransientCanvasState *)cs;
    }
    else {
        DP_TransientCanvasState *tcs = DP_transient_canvas_state_new(cs);
        DP_canvas_state_decref(cs);
        return tcs;
    }
}


static DP_TransientLayerContent *
make_inspect_sublayer(DP_TransientCanvasState *tcs, DP_DrawContext *dc)
{
    int index_count;
    int *indexes = DP_draw_context_layer_indexes(dc, &index_count);
    DP_TransientLayerContent *tlc =
        DP_layer_routes_entry_indexes_transient_content(index_count, indexes,
                                                        tcs);
    DP_TransientLayerContent *sub_tlc;
    DP_TransientLayerProps *sub_tlp;
    DP_transient_layer_content_transient_sublayer(tlc, INSPECT_SUBLAYER_ID,
                                                  &sub_tlc, &sub_tlp);
    DP_transient_layer_props_opacity_set(sub_tlp, DP_BIT15 - DP_BIT15 / 4);
    DP_transient_layer_props_blend_mode_set(sub_tlp, DP_BLEND_MODE_RECOLOR);
    return sub_tlc;
}

static void maybe_add_inspect_sublayer(DP_TransientCanvasState *tcs,
                                       DP_DrawContext *dc,
                                       unsigned int context_id, int xtiles,
                                       int ytiles, DP_LayerContent *lc)
{
    DP_TransientLayerContent *sub_tlc = NULL;
    for (int y = 0; y < ytiles; ++y) {
        for (int x = 0; x < xtiles; ++x) {
            DP_Tile *t = DP_layer_content_tile_at_noinc(lc, x, y);
            if (t && DP_tile_context_id(t) == context_id) {
                if (!sub_tlc) {
                    sub_tlc = make_inspect_sublayer(tcs, dc);
                }
                DP_transient_layer_content_tile_set_noinc(
                    sub_tlc, DP_tile_censored_inc(), y * xtiles + x);
            }
        }
    }
}

static void apply_inspect_recursive(DP_TransientCanvasState *tcs,
                                    DP_DrawContext *dc, unsigned int context_id,
                                    int xtiles, int ytiles, DP_LayerList *ll)
{
    int count = DP_layer_list_count(ll);
    DP_draw_context_layer_indexes_push(dc);
    for (int i = 0; i < count; ++i) {
        DP_draw_context_layer_indexes_set(dc, i);
        DP_LayerListEntry *lle = DP_layer_list_at_noinc(ll, i);
        if (DP_layer_list_entry_is_group(lle)) {
            DP_LayerGroup *lg = DP_layer_list_entry_group_noinc(lle);
            DP_LayerList *child_lle = DP_layer_group_children_noinc(lg);
            apply_inspect_recursive(tcs, dc, context_id, xtiles, ytiles,
                                    child_lle);
        }
        else {
            DP_LayerContent *lc = DP_layer_list_entry_content_noinc(lle);
            maybe_add_inspect_sublayer(tcs, dc, context_id, xtiles, ytiles, lc);
        }
    }
    DP_draw_context_layer_indexes_pop(dc);
}

static DP_CanvasState *apply_inspect(DP_PaintEngine *pe, DP_CanvasState *cs)
{
    unsigned int context_id = pe->local_view.inspect_context_id;
    if (context_id == 0) {
        return cs;
    }
    else {
        DP_TransientCanvasState *tcs = get_or_make_transient_canvas_state(cs);
        DP_DrawContext *dc = pe->preview_dc;
        DP_draw_context_layer_indexes_clear(dc);
        DP_TileCounts tile_counts = DP_tile_counts_round(
            DP_canvas_state_width(cs), DP_canvas_state_height(cs));
        apply_inspect_recursive(tcs, dc, context_id, tile_counts.x,
                                tile_counts.y,
                                DP_canvas_state_layers_noinc(cs));
        return (DP_CanvasState *)tcs;
    }
}


static void set_local_view_layer_props_recursive(
    DP_TransientCanvasState *tcs, DP_DrawContext *dc, int active_layer_id,
    DP_LayerViewMode layer_view_mode, bool reveal_censored,
    DP_LayerPropsList *lpl)
{
    int count = DP_layer_props_list_count(lpl);
    DP_draw_context_layer_indexes_push(dc);
    for (int i = 0; i < count; ++i) {
        DP_draw_context_layer_indexes_set(dc, i);
        DP_LayerProps *lp = DP_layer_props_list_at_noinc(lpl, i);
        DP_LayerPropsList *child_lpl = DP_layer_props_children_noinc(lp);

        bool hide_layer;
        DP_LayerViewMode child_layer_view_mode;
        switch (layer_view_mode) {
        case DP_LAYER_VIEW_MODE_SOLO:
            if (DP_layer_props_id(lp) == active_layer_id) {
                hide_layer = false;
                child_layer_view_mode = DP_LAYER_VIEW_MODE_NORMAL;
            }
            else {
                hide_layer = !child_lpl;
                child_layer_view_mode = layer_view_mode;
            }
            break;
        default:
            hide_layer = false;
            child_layer_view_mode = layer_view_mode;
            break;
        }

        bool change_censored = reveal_censored && DP_layer_props_censored(lp);

        if (hide_layer || change_censored) {
            int index_count;
            int *indexes = DP_draw_context_layer_indexes(dc, &index_count);
            DP_TransientLayerProps *tlp =
                DP_layer_routes_entry_indexes_transient_props(index_count,
                                                              indexes, tcs);
            if (hide_layer) {
                DP_transient_layer_props_hidden_by_view_mode_set(tlp, true);
            }
            if (change_censored) {
                DP_transient_layer_props_censored_set(tlp, false);
            }
        }

        if (child_lpl) {
            set_local_view_layer_props_recursive(tcs, dc, active_layer_id,
                                                 child_layer_view_mode,
                                                 reveal_censored, child_lpl);
        }
    }
    DP_draw_context_layer_indexes_pop(dc);
}

static void set_hidden_layer_props(DP_PaintEngine *pe,
                                   DP_TransientCanvasState *tcs, int used)
{
    int *layer_ids = pe->local_view.hidden_layers.layer_ids;
    DP_LayerRoutes *lr = DP_transient_canvas_state_layer_routes_noinc(tcs);
    // Using a while loop since we might purge elements along the way.
    int i = 0;
    while (i < used) {
        int layer_id = layer_ids[i];
        DP_LayerRoutesEntry *lre = DP_layer_routes_search(lr, layer_id);
        if (lre) {
            DP_TransientLayerProps *tlp =
                DP_layer_routes_entry_transient_props(lre, tcs);
            DP_transient_layer_props_hidden_set(tlp, true);
            ++i;
        }
        else {
            remove_hidden_layer(pe, i);
            --used;
        }
    }
}

static DP_CanvasState *set_local_layer_props(DP_PaintEngine *pe,
                                             DP_CanvasState *cs)
{
    DP_TransientCanvasState *tcs = get_or_make_transient_canvas_state(cs);

    DP_LayerViewMode layer_view_mode = pe->local_view.layer_view_mode;
    bool reveal_censored = pe->local_view.reveal_censored;
    if (layer_view_mode != DP_LAYER_VIEW_MODE_NORMAL || reveal_censored) {
        DP_DrawContext *dc = pe->preview_dc;
        DP_draw_context_layer_indexes_clear(dc);
        set_local_view_layer_props_recursive(
            tcs, dc, pe->local_view.active_layer_id, layer_view_mode,
            reveal_censored, DP_transient_canvas_state_layer_props_noinc(tcs));
    }

    int hidden_layers_used = pe->local_view.hidden_layers.used;
    if (hidden_layers_used != 0) {
        set_hidden_layer_props(pe, tcs, hidden_layers_used);
    }

    // We remember the root layer props list for later and then jam it into
    // subsequent canvas states. That'll make them diff correctly instead of
    // reporting a layer props change on every tick.
    DP_layer_props_list_decref_nullable(pe->local_view.lpl);
    pe->local_view.lpl =
        DP_layer_props_list_incref(DP_transient_layer_props_list_persist(
            DP_transient_canvas_state_transient_layer_props(tcs, 0)));
    return (DP_CanvasState *)tcs;
}

static DP_CanvasState *apply_local_layer_props(DP_PaintEngine *pe,
                                               DP_CanvasState *cs)
{
    DP_LayerPropsList *lpl = DP_canvas_state_layer_props_noinc(cs);
    // This function here may replace the layer props entirely, so there can't
    // have been any meddling with it beforehand. Any layer props changes must
    // be part of this function so that they're cached properly.
    DP_ASSERT(!DP_layer_props_list_transient(lpl));
    DP_LayerPropsList *prev_lpl = pe->local_view.prev_lpl;
    if (lpl == prev_lpl) {
        // If our local view doesn't have anything to change about the canvas
        // state, we don't need to replace anything in the target state either.
        bool keep_layer_props =
            pe->local_view.layer_view_mode == DP_LAYER_VIEW_MODE_NORMAL
            && !pe->local_view.reveal_censored
            && pe->local_view.hidden_layers.used == 0;
        if (keep_layer_props) {
            return cs;
        }
        else {
            DP_TransientCanvasState *tcs =
                get_or_make_transient_canvas_state(cs);
            DP_transient_canvas_state_layer_props_set_inc(tcs,
                                                          pe->local_view.lpl);
            return (DP_CanvasState *)tcs;
        }
    }
    else {
        DP_layer_props_list_decref_nullable(prev_lpl);
        pe->local_view.prev_lpl = DP_layer_props_list_incref(lpl);
        return set_local_layer_props(pe, cs);
    }
}

static void
emit_changes(DP_PaintEngine *pe, DP_CanvasState *prev, DP_CanvasState *cs,
             DP_UserCursorBuffer *ucb, DP_PaintEngineResizedFn resized,
             DP_CanvasDiffEachPosFn tile_changed,
             DP_PaintEngineLayerPropsChangedFn layer_props_changed,
             DP_PaintEngineAnnotationsChangedFn annotations_changed,
             DP_PaintEngineDocumentMetadataChangedFn document_metadata_changed,
             DP_PaintEngineCursorMovedFn cursor_moved, void *user)
{
    int prev_width = DP_canvas_state_width(prev);
    int prev_height = DP_canvas_state_height(prev);
    int width = DP_canvas_state_width(cs);
    int height = DP_canvas_state_height(cs);
    if (prev_width != width || prev_height != height) {
        resized(user,
                DP_canvas_state_offset_x(prev) - DP_canvas_state_offset_x(cs),
                DP_canvas_state_offset_y(prev) - DP_canvas_state_offset_y(cs),
                prev_width, prev_height);
    }

    DP_CanvasDiff *diff = pe->diff;
    DP_canvas_state_diff(cs, prev, diff);
    DP_canvas_diff_each_pos(diff, tile_changed, user);

    if (DP_canvas_diff_layer_props_changed_reset(diff)) {
        layer_props_changed(user, DP_canvas_state_layer_props_noinc(cs));
    }

    DP_AnnotationList *al = DP_canvas_state_annotations_noinc(cs);
    if (al != DP_canvas_state_annotations_noinc(prev)) {
        annotations_changed(user, al);
    }

    DP_DocumentMetadata *dm = DP_canvas_state_metadata_noinc(cs);
    if (dm != DP_canvas_state_metadata_noinc(prev)) {
        document_metadata_changed(user, dm);
    }

    int cursors_count = ucb->count;
    for (int i = 0; i < cursors_count; ++i) {
        DP_UserCursor *uc = &ucb->cursors[i];
        cursor_moved(user, uc->context_id, uc->layer_id, uc->x, uc->y);
    }
}

void DP_paint_engine_tick(
    DP_PaintEngine *pe, DP_PaintEngineCatchupFn catchup,
    DP_PaintEngineResizedFn resized, DP_CanvasDiffEachPosFn tile_changed,
    DP_PaintEngineLayerPropsChangedFn layer_props_changed,
    DP_PaintEngineAnnotationsChangedFn annotations_changed,
    DP_PaintEngineDocumentMetadataChangedFn document_metadata_changed,
    DP_PaintEngineTimelineChangedFn timeline_changed,
    DP_PaintEngineCursorMovedFn cursor_moved, void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(catchup);
    DP_ASSERT(resized);
    DP_ASSERT(tile_changed);
    DP_ASSERT(layer_props_changed);

    int progress = DP_atomic_xch(&pe->catchup, -1);
    if (progress != -1) {
        catchup(user, progress);
    }

    DP_CanvasState *prev_history_cs = pe->history_cs;
    DP_UserCursorBuffer *ucb = (DP_UserCursorBuffer *)pe->buffers;
    DP_CanvasState *next_history_cs =
        DP_canvas_history_compare_and_get(pe->ch, prev_history_cs, ucb);
    if (next_history_cs) {
        DP_canvas_state_decref(prev_history_cs);
        pe->history_cs = next_history_cs;
    }

    DP_PaintEnginePreview *next_preview =
        DP_atomic_ptr_xch(&pe->next_preview, NULL);
    if (next_preview) {
        free_preview(pe->preview);
        pe->preview = next_preview == &null_preview ? NULL : next_preview;
    }

    bool local_view_changed = !pe->local_view.prev_lpl;

    if (next_history_cs || next_preview || local_view_changed) {
        // Previews, hidden layers etc. are local changes, so we have to apply
        // them on top of the canvas state we got out of the history.
        DP_CanvasState *prev_view_cs = pe->view_cs;
        DP_CanvasState *next_view_cs = apply_local_layer_props(
            pe, apply_inspect(pe, apply_preview(pe, pe->history_cs)));
        pe->view_cs = next_view_cs;
        emit_changes(pe, prev_view_cs, next_view_cs, ucb, resized, tile_changed,
                     layer_props_changed, annotations_changed,
                     document_metadata_changed, cursor_moved, user);
        DP_canvas_state_decref(prev_view_cs);
    }
}

void DP_paint_engine_prepare_render(DP_PaintEngine *pe,
                                    DP_PaintEngineRenderSizeFn render_size,
                                    void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(render_size);
    DP_CanvasState *cs = pe->view_cs;
    int width = DP_canvas_state_width(cs);
    int height = DP_canvas_state_height(cs);
    render_size(user, width, height);

    DP_TransientLayerContent *tlc = pe->tlc;
    bool render_size_changed = width != DP_transient_layer_content_width(tlc)
                            || height != DP_transient_layer_content_height(tlc);
    if (render_size_changed) {
        DP_transient_layer_content_decref(tlc);
        pe->tlc = DP_transient_layer_content_new_init(width, height, NULL);
    }
}

static void render_pos(void *user, int x, int y)
{
    struct DP_PaintEngineRenderParams *params = user;
    DP_PaintEngine *pe = params->pe;
    ++pe->render.tiles_waiting;
    struct DP_PaintEngineRenderJobParams job_params = {params, x, y};
    DP_worker_push(pe->render.worker, &job_params);
}

static void wait_for_render(DP_PaintEngine *pe)
{
    int n = pe->render.tiles_waiting;
    if (n != 0) {
        pe->render.tiles_waiting = 0;
        DP_SEMAPHORE_MUST_WAIT_N(pe->render.tiles_done_sem, n);
    }
}

void DP_paint_engine_render_everything(DP_PaintEngine *pe,
                                       DP_PaintEngineRenderTileFn render_tile,
                                       void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(render_tile);
    struct DP_PaintEngineRenderParams params = {
        pe, DP_tile_count_round(DP_canvas_state_width(pe->view_cs)),
        render_tile, user};
    DP_canvas_diff_each_pos_reset(pe->diff, render_pos, &params);
    wait_for_render(pe);
}

void DP_paint_engine_render_tile_bounds(DP_PaintEngine *pe, int tile_left,
                                        int tile_top, int tile_right,
                                        int tile_bottom,
                                        DP_PaintEngineRenderTileFn render_tile,
                                        void *user)
{
    DP_ASSERT(pe);
    DP_ASSERT(render_tile);
    struct DP_PaintEngineRenderParams params = {
        pe, DP_tile_count_round(DP_canvas_state_width(pe->view_cs)),
        render_tile, user};
    DP_canvas_diff_each_pos_tile_bounds_reset(pe->diff, tile_left, tile_top,
                                              tile_right, tile_bottom,
                                              render_pos, &params);
    wait_for_render(pe);
}


static void sync_preview(DP_PaintEngine *pe, DP_PaintEnginePreview *preview)
{
    // Make the preview go through the paint engine so that there's less jerking
    // as previews are created and cleared. There may still be flickering, but
    // it won't look like transforms undo themselves for a moment.
    DP_Message *msg = DP_msg_internal_preview_new(0, preview);
    DP_MUTEX_MUST_LOCK(pe->queue_mutex);
    DP_message_queue_push_noinc(&pe->local_queue, msg);
    DP_SEMAPHORE_MUST_POST(pe->queue_sem);
    DP_MUTEX_MUST_UNLOCK(pe->queue_mutex);
}

static void set_preview(DP_PaintEngine *pe, DP_PaintEnginePreview *preview,
                        DP_PaintEnginePreviewRenderFn render,
                        DP_PaintEnginePreviewDisposeFn dispose)
{
    DP_CanvasState *cs = pe->view_cs;
    preview->initial_offset_x = DP_canvas_state_offset_x(cs);
    preview->initial_offset_y = DP_canvas_state_offset_y(cs);
    preview->render = render;
    preview->dispose = dispose;
    sync_preview(pe, preview);
}


static DP_LayerContent *get_cut_preview_content(DP_PaintEngineCutPreview *pecp,
                                                DP_CanvasState *cs,
                                                int offset_x, int offset_y)
{
    DP_LayerContent *lc = pecp->lc;
    int canvas_width = DP_canvas_state_width(cs);
    int canvas_height = DP_canvas_state_height(cs);
    bool needs_render = !lc || DP_layer_content_width(lc) != canvas_width
                     || DP_layer_content_height(lc) != canvas_height;
    if (needs_render) {
        DP_layer_content_decref_nullable(lc);
        DP_TransientLayerContent *tlc = DP_transient_layer_content_new_init(
            canvas_width, canvas_height, NULL);

        int left = pecp->x + offset_x;
        int top = pecp->y + offset_y;
        int width = pecp->width;
        int height = pecp->height;
        int right =
            DP_min_int(DP_transient_layer_content_width(tlc), left + width);
        int bottom =
            DP_min_int(DP_transient_layer_content_height(tlc), top + height);

        if (pecp->have_mask) {
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    uint8_t a = pecp->mask[(y - top) * width + (x - left)];
                    if (a != 0) {
                        DP_transient_layer_content_pixel_at_set(
                            tlc, 0, x, y,
                            (DP_Pixel15){0, 0, 0, DP_channel8_to_15(a)});
                    }
                }
            }
        }
        else {
            DP_transient_layer_content_fill_rect(
                tlc, 0, DP_BLEND_MODE_REPLACE, left, top, right, bottom,
                (DP_UPixel15){0, 0, 0, DP_BIT15});
        }

        return pecp->lc = DP_transient_layer_content_persist(tlc);
    }
    else {
        return lc;
    }
}

static DP_CanvasState *cut_preview_render(DP_PaintEnginePreview *preview,
                                          DP_CanvasState *cs,
                                          DP_UNUSED DP_DrawContext *dc,
                                          int offset_x, int offset_y)
{
    DP_PaintEngineCutPreview *pecp = (DP_PaintEngineCutPreview *)preview;
    int layer_id = pecp->layer_id;
    DP_LayerRoutes *lr = DP_canvas_state_layer_routes_noinc(cs);
    DP_LayerRoutesEntry *lre = DP_layer_routes_search(lr, layer_id);
    if (!lre || DP_layer_routes_entry_is_group(lre)) {
        return DP_canvas_state_incref(cs);
    }

    DP_TransientCanvasState *tcs = DP_transient_canvas_state_new(cs);
    if (!pecp->lp) {
        DP_TransientLayerProps *tlp =
            DP_transient_layer_props_new_init(PREVIEW_SUBLAYER_ID, false);
        DP_transient_layer_props_blend_mode_set(tlp, DP_BLEND_MODE_ERASE);
        pecp->lp = DP_transient_layer_props_persist(tlp);
    }

    DP_TransientLayerContent *tlc =
        DP_layer_routes_entry_transient_content(lre, tcs);
    DP_transient_layer_content_sublayer_insert_inc(
        tlc, get_cut_preview_content(pecp, cs, offset_x, offset_y), pecp->lp);
    return (DP_CanvasState *)tcs;
}

static void cut_preview_dispose(DP_PaintEnginePreview *preview)
{
    DP_PaintEngineCutPreview *pecp = (DP_PaintEngineCutPreview *)preview;
    DP_layer_props_decref_nullable(pecp->lp);
    DP_layer_content_decref_nullable(pecp->lc);
}

void DP_paint_engine_preview_cut(DP_PaintEngine *pe, int layer_id, int x, int y,
                                 int width, int height,
                                 const DP_Pixel8 *mask_or_null)
{
    DP_ASSERT(pe);
    int mask_count = mask_or_null ? width * height : 0;
    DP_PaintEngineCutPreview *pecp = DP_malloc(DP_FLEX_SIZEOF(
        DP_PaintEngineCutPreview, mask, DP_int_to_size(mask_count)));
    pecp->lc = NULL;
    pecp->lp = NULL;
    pecp->layer_id = layer_id;
    pecp->x = x;
    pecp->y = y;
    pecp->width = width;
    pecp->height = height;
    pecp->have_mask = mask_or_null;
    for (int i = 0; i < mask_count; ++i) {
        pecp->mask[i] = mask_or_null[i].a;
    }
    set_preview(pe, &pecp->parent, cut_preview_render, cut_preview_dispose);
}


static DP_CanvasState *dabs_preview_render(DP_PaintEnginePreview *preview,
                                           DP_CanvasState *cs,
                                           DP_DrawContext *dc, int offset_x,
                                           int offset_y)
{
    DP_PaintEngineDabsPreview *pedp = (DP_PaintEngineDabsPreview *)preview;
    int layer_id = pedp->layer_id;
    DP_LayerRoutes *lr = DP_canvas_state_layer_routes_noinc(cs);
    DP_LayerRoutesEntry *lre = DP_layer_routes_search(lr, layer_id);
    if (!lre || DP_layer_routes_entry_is_group(lre)) {
        return DP_canvas_state_incref(cs);
    }

    DP_TransientCanvasState *tcs = DP_transient_canvas_state_new(cs);
    DP_TransientLayerContent *tlc =
        DP_layer_routes_entry_transient_content(lre, tcs);
    DP_TransientLayerContent *sub_tlc = NULL;

    int count = pedp->count;
    DP_PaintDrawDabsParams params = {0};
    for (int i = 0; i < count; ++i) {
        DP_Message *msg = pedp->messages[i];
        DP_MessageType type = DP_message_type(msg);
        switch (type) {
        case DP_MSG_DRAW_DABS_CLASSIC: {
            DP_MsgDrawDabsClassic *mddc = DP_message_internal(msg);
            params.origin_x = DP_msg_draw_dabs_classic_x(mddc);
            params.origin_y = DP_msg_draw_dabs_classic_y(mddc);
            params.color = DP_msg_draw_dabs_classic_color(mddc);
            params.blend_mode = DP_msg_draw_dabs_classic_mode(mddc);
            params.indirect = DP_msg_draw_dabs_classic_indirect(mddc);
            params.classic.dabs =
                DP_msg_draw_dabs_classic_dabs(mddc, &params.dab_count);
            break;
        }
        case DP_MSG_DRAW_DABS_PIXEL:
        case DP_MSG_DRAW_DABS_PIXEL_SQUARE: {
            DP_MsgDrawDabsPixel *mddp = DP_message_internal(msg);
            params.origin_x = DP_msg_draw_dabs_pixel_x(mddp);
            params.origin_y = DP_msg_draw_dabs_pixel_y(mddp);
            params.color = DP_msg_draw_dabs_pixel_color(mddp);
            params.blend_mode = DP_msg_draw_dabs_pixel_mode(mddp);
            params.indirect = DP_msg_draw_dabs_pixel_indirect(mddp);
            params.pixel.dabs =
                DP_msg_draw_dabs_pixel_dabs(mddp, &params.dab_count);
            break;
        }
        case DP_MSG_DRAW_DABS_MYPAINT: {
            DP_MsgDrawDabsMyPaint *mddmp = DP_message_internal(msg);
            params.origin_x = DP_msg_draw_dabs_mypaint_x(mddmp);
            params.origin_y = DP_msg_draw_dabs_mypaint_y(mddmp);
            params.color = DP_msg_draw_dabs_mypaint_color(mddmp);
            params.blend_mode = DP_BLEND_MODE_NORMAL_AND_ERASER;
            params.indirect = false;
            params.mypaint.dabs =
                DP_msg_draw_dabs_mypaint_dabs(mddmp, &params.dab_count);
            params.mypaint.lock_alpha =
                DP_msg_draw_dabs_mypaint_lock_alpha(mddmp);
            // TODO colorize and posterize when implemented into the protocol
            break;
        }
        default:
            continue;
        }

        if (params.indirect) {
            if (!sub_tlc) {
                DP_TransientLayerProps *tlp;
                DP_transient_layer_content_transient_sublayer(
                    tlc, PREVIEW_SUBLAYER_ID, &sub_tlc, &tlp);
                DP_transient_layer_props_blend_mode_set(tlp, params.blend_mode);
                DP_transient_layer_props_opacity_set(
                    tlp, DP_channel8_to_15(DP_uint32_to_uint8(
                             (params.color & 0xff000000) >> 24)));
            }
            params.blend_mode = DP_BLEND_MODE_NORMAL;
        }

        params.type = (int)type;
        params.origin_x += offset_x;
        params.origin_y += offset_y;
        DP_paint_draw_dabs(dc, &params, params.indirect ? sub_tlc : tlc);
    }

    return (DP_CanvasState *)tcs;
}

static void dabs_preview_dispose(DP_PaintEnginePreview *preview)
{
    DP_PaintEngineDabsPreview *pedp = (DP_PaintEngineDabsPreview *)preview;
    int count = pedp->count;
    for (int i = 0; i < count; ++i) {
        DP_message_decref(pedp->messages[i]);
    }
}

void DP_paint_engine_preview_dabs_inc(DP_PaintEngine *pe, int layer_id,
                                      int count, DP_Message **messages)
{
    DP_ASSERT(pe);
    if (count > 0) {
        DP_PaintEngineDabsPreview *pedp = DP_malloc(DP_FLEX_SIZEOF(
            DP_PaintEngineDabsPreview, messages, DP_int_to_size(count)));
        pedp->layer_id = layer_id;
        pedp->count = count;
        for (int i = 0; i < count; ++i) {
            pedp->messages[i] = DP_message_incref(messages[i]);
        }
        set_preview(pe, &pedp->parent, dabs_preview_render,
                    dabs_preview_dispose);
    }
}


void DP_paint_engine_preview_clear(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    sync_preview(pe, &null_preview);
}


DP_CanvasState *DP_paint_engine_canvas_state_inc(DP_PaintEngine *pe)
{
    DP_ASSERT(pe);
    return DP_canvas_state_incref(pe->view_cs);
}
