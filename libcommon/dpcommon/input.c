/*
 * Copyright (c) 2022 askmeaboutloom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "input.h"
#include "common.h"
#include "conversions.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>


struct DP_Input {
    const DP_InputMethods *methods;
    alignas(max_align_t) unsigned char internal[];
};

DP_Input *DP_input_new(DP_InputInitFn init, void *arg, size_t internal_size)
{
    DP_ASSERT(init);
    DP_ASSERT(internal_size <= SIZE_MAX - sizeof(DP_Input));
    DP_Input *input = DP_malloc_zeroed(sizeof(*input) + internal_size);
    input->methods = init(input->internal, arg);
    if (input->methods) {
        DP_ASSERT(input->methods->read);
        return input;
    }
    else {
        DP_free(input);
        return NULL;
    }
}

void DP_input_free(DP_Input *input)
{
    if (input) {
        void (*dispose)(void *) = input->methods->dispose;
        if (dispose) {
            dispose(input->internal);
        }
        DP_free(input);
    }
}

size_t DP_input_read(DP_Input *input, void *buffer, size_t size,
                     bool *out_error)
{
    if (buffer && size > 0) {
        bool error = false;
        size_t result =
            input->methods->read(input->internal, buffer, size, &error);
        if (out_error) {
            *out_error = error;
        }
        return result;
    }
    else {
        return 0;
    }
}

bool DP_input_rewind_by(DP_Input *input, size_t size)
{
    DP_ASSERT(input);
    if (size > 0) {
        bool (*rewind_by)(void *, size_t) = input->methods->rewind_by;
        if (rewind_by) {
            return rewind_by(input->internal, size);
        }
        else {
            DP_error_set("Rewind not supported");
            return false;
        }
    }
    else {
        return true;
    }
}


typedef struct DP_FileInputState {
    FILE *fp;
    bool close;
} DP_FileInputState;

static size_t file_input_read(void *internal, void *buffer, size_t size,
                              bool *out_error)
{
    FILE *fp = *((FILE **)internal);
    size_t result = fread(buffer, 1, size, fp);
    if (result != size && ferror(fp)) {
        *out_error = true;
        DP_error_set("File input read error: %s", strerror(errno));
    }
    return result;
}

static bool file_input_rewind_by(void *internal, size_t size)
{
    DP_FileInputState *state = internal;
    long offset = -DP_size_to_long(size);
    if (fseek(state->fp, offset, SEEK_CUR) == 0) {
        return true;
    }
    else {
        DP_error_set("File input could not rewind by %" DP_PZU ": %s",
                     DP_PSZ(size), strerror(errno));
        return false;
    }
}

static void file_input_dispose(void *internal)
{
    DP_FileInputState *state = internal;
    if (state->close && fclose(state->fp) != 0) {
        DP_error_set("File input close error: %s", strerror(errno));
    }
}

static const DP_InputMethods file_input_methods = {
    file_input_read,
    file_input_rewind_by,
    file_input_dispose,
};

const DP_InputMethods *file_input_init(void *internal, void *arg)
{
    *((DP_FileInputState *)internal) = *((DP_FileInputState *)arg);
    return &file_input_methods;
}

DP_Input *DP_file_input_new(FILE *fp, bool close)
{
    DP_FileInputState state = {fp, close};
    return DP_input_new(file_input_init, &state, sizeof(state));
}

DP_Input *DP_file_input_new_from_path(const char *path)
{
    DP_ASSERT(path);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        return DP_file_input_new(fp, true);
    }
    else {
        DP_error_set("Can't open '%s': %s", path, strerror(errno));
        return NULL;
    }
}


typedef struct DP_MemInputState {
    void *buffer;
    size_t size;
    DP_MemInputFreeFn free;
    void *free_arg;
    size_t pos;
} DP_MemInputState;

static size_t mem_input_read(void *internal, void *buffer, size_t size,
                             DP_UNUSED bool *out_error)
{
    DP_MemInputState *state = internal;
    DP_ASSERT(state->pos <= state->size);
    size_t left = state->size - state->pos;
    size_t read = size <= left ? size : left;
    memcpy(buffer, (unsigned char *)state->buffer + state->pos, read);
    state->pos += read;
    return read;
}

static bool mem_input_rewind_by(void *internal, size_t size)
{
    DP_MemInputState *state = internal;
    if (state->pos >= size) {
        state->pos -= size;
        return true;
    }
    else {
        DP_error_set("Mem input at position %" DP_PZU
                     " can't be rewound by %" DP_PZU,
                     DP_PSZ(state->pos), DP_PSZ(size));
        return false;
    }
}

static void mem_input_dispose(void *internal)
{
    DP_MemInputState *state = internal;
    DP_MemInputFreeFn free_fn = state->free;
    if (free_fn) {
        free_fn(state->buffer, state->size, state->free_arg);
    }
}

static const DP_InputMethods mem_input_methods = {
    mem_input_read,
    mem_input_rewind_by,
    mem_input_dispose,
};

const DP_InputMethods *mem_input_init(void *internal, void *arg)
{
    *((DP_MemInputState *)internal) = *((DP_MemInputState *)arg);
    return &mem_input_methods;
}

DP_Input *DP_mem_input_new(void *buffer, size_t size, DP_MemInputFreeFn free,
                           void *free_arg)
{
    DP_MemInputState state = {buffer, size, free, free_arg, 0};
    return DP_input_new(mem_input_init, &state, sizeof(state));
}

static void mem_input_free(void *buffer, DP_UNUSED size_t size,
                           DP_UNUSED void *free_arg)
{
    DP_free(buffer);
}

DP_Input *DP_mem_input_new_free_on_close(void *buffer, size_t size)
{
    return DP_mem_input_new(buffer, size, mem_input_free, NULL);
}

DP_Input *DP_mem_input_new_keep_on_close(const void *buffer, size_t size)
{
    return DP_mem_input_new((void *)buffer, size, NULL, NULL);
}
