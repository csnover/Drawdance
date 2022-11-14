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
#include "output.h"
#include "common.h"
#include <errno.h>

#define DP_MEM_OUTPUT_MIN_CAPACITY 32


struct DP_Output {
    const DP_OutputMethods *methods;
    alignas(max_align_t) unsigned char internal[];
};

DP_Output *DP_output_new(DP_OutputInitFn init, void *arg, size_t internal_size)
{
    DP_ASSERT(init);
    DP_ASSERT(internal_size <= SIZE_MAX - sizeof(DP_Output));
    DP_Output *output = DP_malloc_zeroed(sizeof(*output) + internal_size);
    output->methods = init(output->internal, arg);
    if (output->methods) {
        DP_ASSERT(output->methods->write);
        return output;
    }
    else {
        DP_free(output);
        return NULL;
    }
}

void DP_output_free(DP_Output *output)
{
    if (output) {
        void (*dispose)(void *) = output->methods->dispose;
        if (dispose) {
            dispose(output->internal);
        }
        DP_free(output);
    }
}

bool DP_output_write(DP_Output *output, const void *buffer, size_t size)
{
    if (buffer && size > 0) {
        DP_ASSERT(output);
        return output->methods->write(output->internal, buffer, size) == size;
    }
    else {
        return true;
    }
}

bool DP_output_print(DP_Output *output, const char *string)
{
    return DP_output_write(output, string, string ? strlen(string) : 0);
}

bool DP_output_vformat(DP_Output *output, const char *fmt, va_list ap)
{
    DP_ASSERT(fmt);
    char *buffer = DP_vformat(fmt, ap);
    bool result = DP_output_print(output, buffer);
    DP_free(buffer);
    return result;
}

bool DP_output_format(DP_Output *output, const char *fmt, ...)
{
    DP_ASSERT(output);
    DP_ASSERT(fmt);
    va_list ap;
    va_start(ap, fmt);
    bool result = DP_output_vformat(output, fmt, ap);
    va_end(ap);
    return result;
}

bool DP_output_clear(DP_Output *output)
{
    DP_ASSERT(output);
    bool (*clear)(void *) = output->methods->clear;
    if (clear) {
        return clear(output->internal);
    }
    else {
        DP_error_set("Clearing output not supported");
        return false;
    }
}

bool DP_output_flush(DP_Output *output)
{
    DP_ASSERT(output);
    bool (*flush)(void *) = output->methods->flush;
    return flush ? flush(output->internal) : true;
}


typedef struct DP_FileOutputState {
    FILE *fp;
    bool close;
} DP_FileOutputState;

static size_t file_output_write(void *internal, const void *buffer, size_t size)
{
    DP_FileOutputState *state = internal;
    size_t written = fwrite(buffer, 1, size, state->fp);
    if (written != size) {
        DP_error_set("File output wrote %" DP_PZU
                     " instead of expected %" DP_PZU " bytes: %s",
                     DP_PSZ(size), DP_PSZ(written), strerror(errno));
    }
    return written;
}

static bool file_output_flush(void *internal)
{
    DP_FileOutputState *state = internal;
    if (fflush(state->fp) == 0) {
        return true;
    }
    else {
        DP_error_set("File output flush error: %s", strerror(errno));
        return false;
    }
}

static void file_output_dispose(void *internal)
{
    DP_FileOutputState *state = internal;
    if (state->close && fclose(state->fp) != 0) {
        DP_error_set("File output close error: %s", strerror(errno));
    }
}

static const DP_OutputMethods file_output_methods = {
    file_output_write,
    NULL,
    file_output_flush,
    file_output_dispose,
};

static const DP_OutputMethods *file_output_init(void *internal, void *arg)
{
    *((DP_FileOutputState *)internal) = *((DP_FileOutputState *)arg);
    return &file_output_methods;
}

DP_Output *DP_file_output_new(FILE *fp, bool close)
{
    DP_FileOutputState state = {fp, close};
    return DP_output_new(file_output_init, &state, sizeof(state));
}

DP_Output *DP_file_output_new_from_path(const char *path)
{
    DP_ASSERT(path);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        return DP_file_output_new(fp, true);
    }
    else {
        DP_error_set("Can't open '%s': %s", path, strerror(errno));
        return NULL;
    }
}


typedef struct DP_MemOutputState {
    unsigned char *buffer;
    size_t capacity, used;
    bool free_on_close;
} DP_MemOutputState;

static size_t mem_output_space(DP_MemOutputState *state)
{
    return state->capacity - state->used - 1u;
}

static size_t mem_output_write(void *internal, const void *buffer, size_t size)
{
    DP_MemOutputState *state = internal;
    if (mem_output_space(state) < size) {
        do {
            state->capacity *= 2u;
        } while (mem_output_space(state) < size);
        state->buffer = DP_realloc(state->buffer, state->capacity);
    }
    memcpy(state->buffer + state->used, buffer, size);
    size_t end = state->used + size;
    state->buffer[end] = '\0';
    state->used = end;
    return size;
}

static bool mem_output_clear(void *internal)
{
    DP_MemOutputState *state = internal;
    state->used = 0;
    state->buffer[0] = '\0';
    return true;
}

static void mem_output_dispose(void *internal)
{
    DP_MemOutputState *state = internal;
    if (state->free_on_close) {
        free(state->buffer);
    }
}

static const DP_OutputMethods mem_output_methods = {
    mem_output_write,
    mem_output_clear,
    NULL,
    mem_output_dispose,
};

static const DP_OutputMethods *mem_output_init(void *internal, void *arg)
{
    *((DP_MemOutputState *)internal) = *((DP_MemOutputState *)arg);
    return &mem_output_methods;
}

static void mem_output_assign_out_vars(DP_Output *output, void ***out_buffer,
                                       size_t **out_size)
{
    DP_MemOutputState *state = (void *)output->internal;
    if (out_buffer) {
        *out_buffer = (void *)&state->buffer;
    }
    if (out_size) {
        *out_size = &state->used;
    }
}

DP_Output *DP_mem_output_new(size_t initial_capacity, bool free_on_close,
                             void ***out_buffer, size_t **out_size)
{
    size_t capacity = initial_capacity < DP_MEM_OUTPUT_MIN_CAPACITY
                        ? DP_MEM_OUTPUT_MIN_CAPACITY
                        : initial_capacity;
    DP_MemOutputState state = {DP_malloc(capacity), capacity, 0, free_on_close};
    DP_Output *output = DP_output_new(mem_output_init, &state, sizeof(state));
    mem_output_assign_out_vars(output, out_buffer, out_size);
    return output;
}
