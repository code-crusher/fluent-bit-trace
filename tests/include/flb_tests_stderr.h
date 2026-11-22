/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2026 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef FLB_TESTS_STDERR_H
#define FLB_TESTS_STDERR_H

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define flb_test_stderr_dup(fd)          _dup(fd)
#define flb_test_stderr_dup2(fd, target) _dup2(fd, target)
#define flb_test_stderr_close(fd)        _close(fd)
#define flb_test_stderr_fileno(stream)   _fileno(stream)
#else
#include <unistd.h>
#define flb_test_stderr_dup(fd)          dup(fd)
#define flb_test_stderr_dup2(fd, target) dup2(fd, target)
#define flb_test_stderr_close(fd)        close(fd)
#define flb_test_stderr_fileno(stream)   fileno(stream)
#endif

/*
 * Redirect stderr into a temporary file so tests can assert on the log
 * messages emitted by a code path. Must be paired with
 * flb_test_stderr_capture_stop().
 */
struct flb_test_stderr_capture {
    FILE *file;
    int   saved_fd;
};

static inline int flb_test_stderr_capture_start(struct flb_test_stderr_capture *capture)
{
    capture->file = tmpfile();
    if (capture->file == NULL) {
        return -1;
    }

    fflush(stderr);

    capture->saved_fd = flb_test_stderr_dup(flb_test_stderr_fileno(stderr));
    if (capture->saved_fd < 0) {
        fclose(capture->file);
        capture->file = NULL;
        return -1;
    }

    flb_test_stderr_dup2(flb_test_stderr_fileno(capture->file),
                         flb_test_stderr_fileno(stderr));

    return 0;
}

static inline size_t flb_test_stderr_capture_stop(struct flb_test_stderr_capture *capture,
                                                  char *buffer, size_t buffer_size)
{
    size_t bytes_read;

    fflush(stderr);
    flb_test_stderr_dup2(capture->saved_fd, flb_test_stderr_fileno(stderr));
    flb_test_stderr_close(capture->saved_fd);

    rewind(capture->file);
    bytes_read = fread(buffer, 1, buffer_size - 1, capture->file);
    buffer[bytes_read] = '\0';
    fclose(capture->file);
    capture->file = NULL;

    return bytes_read;
}

static inline size_t flb_test_count_substring(const char *haystack, const char *needle)
{
    size_t count;
    const char *cursor;

    count = 0;
    cursor = haystack;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += strlen(needle);
    }

    return count;
}

#endif
