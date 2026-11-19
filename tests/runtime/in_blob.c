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

#include <fluent-bit.h>
#include <fluent-bit/flb_compat.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "flb_tests_runtime.h"
#include "../include/flb_tests_tmpdir.h"

/* The recursive scan relies on glob(3) semantics, so the end to end case
 * below only exercises platforms that use the system glob implementation
 * instead of the bundled win32 glob shim.
 */
#ifndef FLB_SYSTEM_WINDOWS

#include <unistd.h>

#define TEST_SCAN_WAIT_SECONDS   6
#define TEST_CAPTURE_MAX_BYTES   (256 * 1024)

struct test_file {
    const char *subdir;   /* directory below the scan root, "" for the root */
    const char *name;     /* file name */
};

/* Tree covered by the "<root>/" followed by the recursion wildcard and
 * "/*" pattern of the test:
 *
 *   root/
 *     a.bin
 *     sub/
 *       c.bin
 *     v1.2/
 *       d.bin
 *
 * "v1.2" is a directory whose name matches the trailing "star-dot-star"
 * style wildcard patterns, which used to drive the scan into an endless
 * recursion (issue #12554).
 */
static struct test_file test_files[] = {
    {"",      "a.bin"},
    {"sub",   "c.bin"},
    {"v1.2",  "d.bin"}
};

static int create_test_tree(char *root, size_t root_size, char **root_dir)
{
    int i;
    int ret;
    char path[PATH_MAX];
    FILE *fp;

    *root_dir = flb_test_tmpdir_cat("/flb-in-blob-XXXXXX");
    if (!TEST_CHECK(*root_dir != NULL)) {
        return -1;
    }

    if (!TEST_CHECK(mkdtemp(*root_dir) != NULL)) {
        flb_free(*root_dir);
        *root_dir = NULL;
        return -1;
    }

    ret = snprintf(root, root_size, "%s/root", *root_dir);
    if (!TEST_CHECK(ret > 0 && (size_t) ret < root_size)) {
        return -1;
    }

    if (!TEST_CHECK(mkdir(root, 0755) == 0)) {
        return -1;
    }

    for (i = 0; i < sizeof(test_files) / sizeof(test_files[0]); i++) {
        if (test_files[i].subdir[0] != '\0') {
            ret = snprintf(path, sizeof(path), "%s/%s",
                           root, test_files[i].subdir);
            if (!TEST_CHECK(ret > 0 && (size_t) ret < sizeof(path))) {
                return -1;
            }

            if (!TEST_CHECK(mkdir(path, 0755) == 0)) {
                return -1;
            }
        }

        ret = snprintf(path, sizeof(path), "%s/%s%s%s",
                       root,
                       test_files[i].subdir,
                       test_files[i].subdir[0] != '\0' ? "/" : "",
                       test_files[i].name);
        if (!TEST_CHECK(ret > 0 && (size_t) ret < sizeof(path))) {
            return -1;
        }

        fp = fopen(path, "w");
        if (!TEST_CHECK(fp != NULL)) {
            return -1;
        }

        fputs("blob test payload\n", fp);
        fclose(fp);
    }

    return 0;
}

static void remove_test_tree(char *root_dir)
{
    int i;
    int ret;
    char path[PATH_MAX];

    if (root_dir == NULL) {
        return;
    }

    for (i = sizeof(test_files) / sizeof(test_files[0]) - 1; i >= 0; i--) {
        ret = snprintf(path, sizeof(path), "%s/root/%s%s",
                       root_dir,
                       test_files[i].subdir[0] != '\0' ?
                           test_files[i].subdir : "",
                       test_files[i].name);
        if (ret > 0 && (size_t) ret < sizeof(path)) {
            remove(path);
        }

        if (test_files[i].subdir[0] != '\0') {
            ret = snprintf(path, sizeof(path), "%s/root/%s",
                           root_dir, test_files[i].subdir);
            if (ret > 0 && (size_t) ret < sizeof(path)) {
                flb_test_rmdir(path);
            }
        }
    }

    ret = snprintf(path, sizeof(path), "%s/root", root_dir);
    if (ret > 0 && (size_t) ret < sizeof(path)) {
        flb_test_rmdir(path);
    }

    ret = snprintf(path, sizeof(path), "%s/blob.db", root_dir);
    if (ret > 0 && (size_t) ret < sizeof(path)) {
        remove(path);
    }

    flb_test_rmdir(root_dir);
    flb_free(root_dir);
}

static int run_blob_scan_capture(char *root, char *root_dir,
                                 char *output, size_t output_size)
{
    int i;
    int ret;
    int in_ffd;
    int out_ffd;
    int stdout_fd;
    size_t bytes_read;
    char db_path[PATH_MAX];
    char pattern[PATH_MAX];
    FILE *capture;
    flb_ctx_t *ctx;

    capture = tmpfile();
    if (!TEST_CHECK(capture != NULL)) {
        return -1;
    }

    fflush(stdout);

    stdout_fd = dup(fileno(stdout));
    if (!TEST_CHECK(stdout_fd != -1)) {
        fclose(capture);
        return -1;
    }

    if (dup2(fileno(capture), fileno(stdout)) == -1) {
        close(stdout_fd);
        fclose(capture);
        return -1;
    }

    ctx = flb_create();
    if (!TEST_CHECK(ctx != NULL)) {
        ret = -1;
        goto restore_stdout;
    }

    ret = flb_service_set(ctx,
                          "Flush", "1",
                          "Grace", "1",
                          "Log_Level", "error",
                          NULL);
    TEST_CHECK(ret == 0);

    in_ffd = flb_input(ctx, (char *) "blob", NULL);
    if (!TEST_CHECK(in_ffd >= 0)) {
        ret = -1;
        goto destroy_ctx;
    }

    ret = snprintf(pattern, sizeof(pattern), "%s/**/*", root);
    if (!TEST_CHECK(ret > 0 && (size_t) ret < sizeof(pattern))) {
        ret = -1;
        goto destroy_ctx;
    }

    ret = snprintf(db_path, sizeof(db_path), "%s/blob.db", root_dir);
    if (!TEST_CHECK(ret > 0 && (size_t) ret < sizeof(db_path))) {
        ret = -1;
        goto destroy_ctx;
    }

    ret = flb_input_set(ctx, in_ffd,
                        "tag", "test_blob",
                        "path", pattern,
                        "database_file", db_path,
                        "scan_refresh_interval", "1s",
                        NULL);
    TEST_CHECK(ret == 0);

    out_ffd = flb_output(ctx, (char *) "stdout", NULL);
    if (!TEST_CHECK(out_ffd >= 0)) {
        ret = -1;
        goto destroy_ctx;
    }

    ret = flb_output_set(ctx, out_ffd, "match", "*", NULL);
    TEST_CHECK(ret == 0);

    ret = flb_start(ctx);
    if (!TEST_CHECK(ret == 0)) {
        ret = -1;
        goto destroy_ctx;
    }

    /* let the scan collector find the files and the flush cycle deliver them */
    for (i = 0; i < TEST_SCAN_WAIT_SECONDS; i++) {
        sleep(1);
    }

    ret = flb_stop(ctx);
    TEST_CHECK(ret == 0);

destroy_ctx:
    flb_destroy(ctx);

restore_stdout:
    fflush(stdout);
    dup2(stdout_fd, fileno(stdout));
    close(stdout_fd);

    rewind(capture);
    bytes_read = fread(output, 1, output_size - 1, capture);
    output[bytes_read] = '\0';

    fclose(capture);

    return ret;
}

static void test_recursive_glob_scan(void)
{
    int i;
    int ret;
    char root[PATH_MAX];
    char expected[PATH_MAX];
    char *output;
    char *root_dir = NULL;

    ret = create_test_tree(root, sizeof(root), &root_dir);
    if (!TEST_CHECK(ret == 0)) {
        remove_test_tree(root_dir);
        return;
    }

    output = flb_malloc(TEST_CAPTURE_MAX_BYTES);
    if (!TEST_CHECK(output != NULL)) {
        remove_test_tree(root_dir);
        return;
    }

    ret = run_blob_scan_capture(root, root_dir,
                                output, TEST_CAPTURE_MAX_BYTES);
    if (!TEST_CHECK(ret == 0)) {
        flb_free(output);
        remove_test_tree(root_dir);
        return;
    }

    for (i = 0; i < sizeof(test_files) / sizeof(test_files[0]); i++) {
        ret = snprintf(expected, sizeof(expected), "%s/%s%s%s",
                       root,
                       test_files[i].subdir,
                       test_files[i].subdir[0] != '\0' ? "/" : "",
                       test_files[i].name);

        if (TEST_CHECK(ret > 0 && (size_t) ret < sizeof(expected))) {
            TEST_CHECK_(strstr(output, expected) != NULL,
                        "scanned blob file %s was not delivered",
                        expected);
        }
    }

    flb_free(output);
    remove_test_tree(root_dir);
}

#endif /* !FLB_SYSTEM_WINDOWS */

TEST_LIST = {
#ifndef FLB_SYSTEM_WINDOWS
    {"recursive_glob_scan", test_recursive_glob_scan},
#endif
    {NULL, NULL}
};
