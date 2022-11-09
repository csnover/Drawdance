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
#include "common.h"
#include "atomic.h"
#include "conversions.h"
#include "threading.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>

DP_ATOMIC_DECLARE_STATIC_SPIN_LOCK(log_lock);

static void log_message(const char *file, int line, const char *level,
                        const char *fmt, va_list ap)
{
    DP_atomic_lock(&log_lock);
    fprintf(stderr, "[%s] %s:%d - ", level, file ? file : "?", line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    DP_atomic_unlock(&log_lock);
}

#define DO_LOG(FILE, LINE, LEVEL, FMT, AP)       \
    do {                                         \
        va_list AP;                              \
        va_start(AP, FMT);                       \
        log_message(FILE, LINE, LEVEL, FMT, AP); \
        va_end(AP);                              \
    } while (0)

#ifndef NDEBUG
void DP_debug_at(const char *file, int line, const char *fmt, ...)
{
    DO_LOG(file, line, "DEBUG", fmt, ap);
}
#endif

void DP_warn_at(const char *file, int line, const char *fmt, ...)
{
    DO_LOG(file, line, "WARNING", fmt, ap);
}

void DP_panic_at(const char *file, int line, const char *fmt, ...)
{
    DO_LOG(file, line, "PANIC", fmt, ap);
    DP_TRAP();
}


void DP_free(void *ptr)
{
    free(ptr);
}

void *DP_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr || size == 0) {
        return ptr;
    }
    else {
        fprintf(stderr, "Allocation of %" DP_PZU " bytes failed\n",
                DP_PSZ(size));
        DP_TRAP();
    }
}

void *DP_realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (new_ptr || size == 0) {
        return new_ptr;
    }
    else {
        fprintf(stderr, "Reallocation of %" DP_PZU " bytes failed\n",
                DP_PSZ(size));
        DP_TRAP();
    }
}


char *DP_vformat(const char *fmt, va_list ap)
{
    va_list aq;

    va_copy(aq, ap);
    int len = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);

    size_t size = DP_int_to_size(len) + 1;
    char *buf = DP_malloc(size);
    vsnprintf(buf, size, fmt, ap);

    return buf;
}

char *DP_format(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *buf = DP_vformat(fmt, ap);
    va_end(ap);
    return buf;
}

char *DP_strdup(const char *str)
{
    if (str) {
        size_t size = strlen(str) + 1;
        char *dup = DP_malloc(size);
        return strncpy(dup, str, size);
    }
    else {
        return NULL;
    }
}


void *DP_slurp(const char *path, size_t *out_length)
{
    DP_ASSERT(path);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        DP_error_set("Can't open '%s': %s", path, strerror(errno));
        return NULL;
    }

    char *buf = NULL;
    if (fseek(fp, 0L, SEEK_END) == -1) {
        DP_error_set("Can't seek end of '%s': %s", path, strerror(errno));
        goto slurp_close;
    }

    long maybe_length = ftell(fp);
    if (maybe_length == -1) {
        DP_error_set("Can't tell length of '%s': %s", path, strerror(errno));
        goto slurp_close;
    }

    if (fseek(fp, 0L, SEEK_SET) == -1L) {
        DP_error_set("Can't seek start of '%s': %s", path, strerror(errno));
        goto slurp_close;
    }

    size_t length = DP_long_to_size(maybe_length);
    size_t size = length + 1;
    buf = DP_malloc(size);
    size_t read = fread(buf, 1, length, fp);
    if (read != length) {
        DP_error_set("Can't read %" DP_PZU " bytes from '%s': got %" DP_PZU
                     " bytes",
                     DP_PSZ(length), path, DP_PSZ(read));
        DP_free(buf);
        buf = NULL;
        goto slurp_close;
    }

    buf[length] = '\0';
    if (out_length) {
        *out_length = length;
    }

slurp_close:
    if (fclose(fp) != 0) {
        DP_warn("Error closing '%s': %s", path, strerror(errno));
    }
    return buf;
}


const char *DP_error(void)
{
    DP_ErrorState error = DP_thread_error_state_get();
    return error.buffer;
}

const char *DP_error_since(unsigned int count)
{
    DP_ErrorState error = DP_thread_error_state_get();
    return count != *error.count ? error.buffer : NULL;
}

void DP_error_set(const char *fmt, ...)
{
    DP_ErrorState error = DP_thread_error_state_get();
    ++*error.count;

    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(error.buffer, error.buffer_size, fmt, ap);
    va_end(ap);

    if (length >= 0) {
        size_t size = DP_int_to_size(length) + 1u;
        if (size > error.buffer_size) {
            error = DP_thread_error_state_resize(size);
            va_start(ap, fmt);
            vsnprintf(error.buffer, error.buffer_size, fmt, ap);
            va_end(ap);
        }
    }

    DP_debug("Set error %u: %s", *error.count,
             error.buffer ? error.buffer : "");
}

unsigned int DP_error_count(void)
{
    DP_ErrorState error = DP_thread_error_state_get();
    return *error.count;
}

void DP_error_count_set(unsigned int count)
{
    DP_ErrorState error = DP_thread_error_state_get();
    *error.count = count;
}

unsigned int DP_error_count_since(unsigned int count)
{
    unsigned int error_count = DP_error_count();
    if (error_count >= count) {
        return error_count - count;
    }
    else {
        // Wraparound of the unsigned int. The wraparound is guaranteed
        // for unsigned numbers by the C standard, so we can rely on it.
        return UINT_MAX - count + error_count + 1;
    }
}
