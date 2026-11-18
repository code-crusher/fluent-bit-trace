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
#include <fluent-bit/flb_time.h>

#include "flb_tests_runtime.h"
#include "../include/flb_tests_tmpdir.h"

#ifdef FLB_HAVE_SQLDB
#include <sqlite3.h>
#endif

/* Number of files created by the test, all of them below a sub directory */
#define BLOB_TEST_EXPECTED_FILES 4
#define BLOB_TEST_WAIT_SECONDS   10

struct blob_test_ctx {
    char root[PATH_MAX];
    char database_file[PATH_MAX];
};

/* Directories are created in order, so parents come before their children */
static const char *blob_test_directories[] = {
    "top",
    "top/nested",
    "top/nested/deeper",
    "top/v1.2",
    NULL
};

static const char *blob_test_files[] = {
    "top/root.bin",
    "top/nested/middle.bin",
    "top/nested/deeper/leaf.bin",
    "top/v1.2/dotted.bin",
    NULL
};

static int blob_test_database_file_count(const char *database_file);

static int blob_test_path(struct blob_test_ctx *ctx,
                          const char *relative_path,
                          char *buffer,
                          size_t buffer_size)
{
    int length;

    length = snprintf(buffer, buffer_size, "%s/%s", ctx->root, relative_path);
    if (length < 0 || (size_t) length >= buffer_size) {
        TEST_MSG("path \"%s\" does not fit in the buffer", relative_path);

        return -1;
    }

    return 0;
}

static int blob_test_ctx_create(struct blob_test_ctx *ctx)
{
    int length;
    char *tmpdir;

    tmpdir = flb_test_env_tmpdir();
    if (!TEST_CHECK(tmpdir != NULL)) {
        TEST_MSG("could not get the temporary directory");

        return -1;
    }

    length = snprintf(ctx->root, sizeof(ctx->root),
                      "%s/flb-in-blob-XXXXXX", tmpdir);
    flb_free(tmpdir);

    if (!TEST_CHECK(length > 0 && (size_t) length < sizeof(ctx->root))) {
        TEST_MSG("temporary directory path is too long");

        return -1;
    }

    if (!TEST_CHECK(mkdtemp(ctx->root) != NULL)) {
        TEST_MSG("mkdtemp(%s) failed", ctx->root);

        return -1;
    }

    length = snprintf(ctx->database_file, sizeof(ctx->database_file),
                      "%s/blob.db", ctx->root);
    if (!TEST_CHECK(length > 0 &&
                    (size_t) length < sizeof(ctx->database_file))) {
        TEST_MSG("database file path is too long");

        flb_test_rmdir(ctx->root);

        return -1;
    }

    return 0;
}

static void blob_test_ctx_destroy(struct blob_test_ctx *ctx)
{
    int index;
    char path[PATH_MAX];

    remove(ctx->database_file);

    for (index = 0; blob_test_files[index] != NULL; index++) {
        if (blob_test_path(ctx, blob_test_files[index],
                           path, sizeof(path)) == 0) {
            remove(path);
        }
    }

    /* Remove the directories from the deepest one to the root */
    for (index = (int) (sizeof(blob_test_directories) /
                        sizeof(blob_test_directories[0])) - 2;
         index >= 0; index--) {
        if (blob_test_path(ctx, blob_test_directories[index],
                           path, sizeof(path)) == 0) {
            flb_test_rmdir(path);
        }
    }

    flb_test_rmdir(ctx->root);
}

static int blob_test_populate(struct blob_test_ctx *ctx)
{
    int index;
    char path[PATH_MAX];
    FILE *file;

    for (index = 0; blob_test_directories[index] != NULL; index++) {
        if (blob_test_path(ctx, blob_test_directories[index],
                           path, sizeof(path)) != 0) {
            return -1;
        }

        if (!TEST_CHECK(flb_test_mkdir(path) == 0)) {
            TEST_MSG("could not create directory %s", path);

            return -1;
        }
    }

    for (index = 0; blob_test_files[index] != NULL; index++) {
        if (blob_test_path(ctx, blob_test_files[index],
                           path, sizeof(path)) != 0) {
            return -1;
        }

        file = fopen(path, "w");
        if (!TEST_CHECK(file != NULL)) {
            TEST_MSG("could not create file %s", path);

            return -1;
        }

        fprintf(file, "blob");
        fclose(file);
    }

    return 0;
}

static int blob_test_run(struct blob_test_ctx *ctx,
                         const char *pattern,
                         int expected_files)
{
    int ret;
    int input_fd;
    int elapsed;
    int count;
    flb_ctx_t *flb;

    flb = flb_create();
    if (!TEST_CHECK(flb != NULL)) {
        return -1;
    }

    ret = flb_service_set(flb,
                          "Flush", "1",
                          "Grace", "1",
                          "Log_Level", "error",
                          NULL);
    if (!TEST_CHECK(ret == 0)) {
        flb_destroy(flb);

        return -1;
    }

    input_fd = flb_input(flb, "blob", NULL);
    if (!TEST_CHECK(input_fd >= 0)) {
        flb_destroy(flb);

        return -1;
    }

    ret = flb_input_set(flb, input_fd,
                        "tag", "test",
                        "path", pattern,
                        "database_file", ctx->database_file,
                        "scan_refresh_interval", "1s",
                        NULL);
    if (!TEST_CHECK(ret == 0)) {
        flb_destroy(flb);

        return -1;
    }

    ret = flb_start(flb);
    if (!TEST_CHECK(ret == 0)) {
        flb_destroy(flb);

        return -1;
    }

    /*
     * Wait for the input collector to register every file. Before the fix a
     * directory matching the trailing pattern made the recursive search call
     * itself with the very same path until the stack overflowed, so the
     * collector never completed a scan.
     */
    for (elapsed = 0; elapsed < BLOB_TEST_WAIT_SECONDS; elapsed++) {
        count = blob_test_database_file_count(ctx->database_file);
        if (count == expected_files) {
            break;
        }

        flb_time_msleep(500);
    }

    flb_stop(flb);
    flb_destroy(flb);

    return 0;
}

static int blob_test_database_file_count(const char *database_file)
{
#ifdef FLB_HAVE_SQLDB
    int result;
    int count;
    sqlite3 *database;
    sqlite3_stmt *statement;

    count = -1;
    database = NULL;

    result = sqlite3_open_v2(database_file, &database,
                             SQLITE_OPEN_READONLY, NULL);
    if (result != SQLITE_OK) {
        if (database != NULL) {
            TEST_MSG("could not open database: %s", sqlite3_errmsg(database));
            sqlite3_close(database);
        }
        else {
            TEST_MSG("could not open database");
        }

        return -1;
    }

    result = sqlite3_prepare_v2(database,
                                "SELECT COUNT(*) FROM in_blob_files",
                                -1, &statement, NULL);
    if (result == SQLITE_OK) {
        if (sqlite3_step(statement) == SQLITE_ROW) {
            count = sqlite3_column_int(statement, 0);
        }

        sqlite3_finalize(statement);
    }
    else {
        TEST_MSG("could not prepare count query: %s", sqlite3_errmsg(database));
    }

    sqlite3_close(database);

    return count;
#else
    (void) database_file;

    return -1;
#endif
}

static void blob_test_scan_pattern(const char *relative_pattern,
                                   int expected_files)
{
    int ret;
    int count;
    char pattern[PATH_MAX];
    struct blob_test_ctx ctx;

    if (!TEST_CHECK(blob_test_ctx_create(&ctx) == 0)) {
        return;
    }

    ret = blob_test_populate(&ctx);
    if (!TEST_CHECK(ret == 0)) {
        blob_test_ctx_destroy(&ctx);

        return;
    }

    if (!TEST_CHECK(blob_test_path(&ctx, relative_pattern,
                                   pattern, sizeof(pattern)) == 0)) {
        blob_test_ctx_destroy(&ctx);

        return;
    }

    if (!TEST_CHECK(blob_test_run(&ctx, pattern, expected_files) == 0)) {
        blob_test_ctx_destroy(&ctx);

        return;
    }

    count = blob_test_database_file_count(ctx.database_file);
    if (!TEST_CHECK(count == expected_files)) {
        TEST_MSG("pattern \"%s\": expected %d discovered files, got %d",
                 relative_pattern, expected_files, count);
    }

    blob_test_ctx_destroy(&ctx);
}

/*
 * A trailing pattern that matches a directory name used to make the
 * recursive search scan the very same directory forever.
 */
void flb_test_recursive_scan(void)
{
    blob_test_scan_pattern("top/**/*", BLOB_TEST_EXPECTED_FILES);
}

/* A dotted directory name also matches the trailing "star dot star" pattern */
void flb_test_recursive_scan_dotted_directory(void)
{
    blob_test_scan_pattern("top/**/*.*", BLOB_TEST_EXPECTED_FILES);
}

TEST_LIST = {
    {"recursive_scan",                  flb_test_recursive_scan},
    {"recursive_scan_dotted_directory", flb_test_recursive_scan_dotted_directory},
    {NULL, NULL}
};
