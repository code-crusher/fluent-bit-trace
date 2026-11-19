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

/*
 * Runtime tests for out_azure_blob local buffering behavior, covering the
 * issues reported in fluent/fluent-bit#12531: the upload timer must not
 * block the worker event loop while retrying against a failing endpoint,
 * and buffered files must not stay memory mapped.
 */

#include <stdlib.h>
#include <string.h>

#include <fluent-bit.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_sds.h>
#include "flb_tests_runtime.h"
#include "../include/flb_tests_tmpdir.h"

#include "../../plugins/out_azure_blob/azure_blob.h"
#include "../../plugins/out_azure_blob/azure_blob_store.h"

#define AZB_TEST_ENDPOINT_FAIL "http://127.0.0.1:1/"

#define AZB_TEST_WAIT_STEP_MS     50
#define AZB_TEST_UPLOAD_TIMEOUT   "2s"

static struct flb_azure_blob *get_azure_blob_context(flb_ctx_t *ctx)
{
    struct flb_output_instance *ins;

    if (ctx == NULL || mk_list_is_empty(&ctx->config->outputs) == 0) {
        return NULL;
    }

    ins = mk_list_entry_first(&ctx->config->outputs,
                              struct flb_output_instance, _head);
    return (struct flb_azure_blob *) ins->context;
}

static char *azb_create_buffer_dir(const char *postfix)
{
    char *buffer_dir;

    buffer_dir = flb_test_tmpdir_cat(postfix);
    if (buffer_dir == NULL) {
        return NULL;
    }

    if (mkdtemp(buffer_dir) == NULL) {
        flb_free(buffer_dir);
        return NULL;
    }

    return buffer_dir;
}

static void azb_timer_retry_setup(flb_ctx_t *ctx, int out_ffd,
                                  const char *buffer_dir)
{
    flb_output_set(ctx, out_ffd, "match", "*", NULL);
    flb_output_set(ctx, out_ffd, "account_name", "devstoreaccount1", NULL);
    flb_output_set(ctx, out_ffd, "container_name", "c1", NULL);
    flb_output_set(ctx, out_ffd, "shared_key",
                   "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/"
                   "KBHBeksoGMGw==", NULL);
    flb_output_set(ctx, out_ffd, "endpoint", AZB_TEST_ENDPOINT_FAIL, NULL);
    flb_output_set(ctx, out_ffd, "blob_type", "blockblob", NULL);
    flb_output_set(ctx, out_ffd, "emulator_mode", "true", NULL);
    flb_output_set(ctx, out_ffd, "buffering_enabled", "true", NULL);
    flb_output_set(ctx, out_ffd, "buffer_dir", buffer_dir, NULL);
    flb_output_set(ctx, out_ffd, "upload_timeout", AZB_TEST_UPLOAD_TIMEOUT, NULL);
    flb_output_set(ctx, out_ffd, "scheduler_max_retries", "2", NULL);
    flb_output_set(ctx, out_ffd, "auto_create_container", "false", NULL);
    flb_output_set(ctx, out_ffd, "unify_tag", "true", NULL);
}

static void push_records(flb_ctx_t *ctx, int in_ffd, int count)
{
    int i;
    int ret;

    for (i = 0; i < count; i++) {
        ret = flb_lib_push(ctx, in_ffd,
                           (char *) "[12345678, {\"key\":\"value\"}]",
                           sizeof("[12345678, {\"key\":\"value\"}]") - 1);
        TEST_CHECK(ret >= 0);
    }
}

/*
 * With buffering enabled and a failing endpoint, the flush path keeps
 * buffering chunks (FLB_OK) instead of being starved behind timer sleeps.
 * Before the fix, the first timer tick slept up to ~13s and flush tasks
 * piled up with no completion.
 */
static void flb_test_buffer_flush_not_blocked_by_timer()
{
    int ret;
    flb_ctx_t *ctx;
    int in_ffd;
    int out_ffd;
    char *buffer_dir;
    struct flb_azure_blob *azb;
    int i;

    buffer_dir = azb_create_buffer_dir("/flb-azb-buffer-blocked-XXXXXX");
    TEST_CHECK(buffer_dir != NULL);
    if (buffer_dir == NULL) {
        return;
    }

    ctx = flb_create();
    TEST_CHECK(ctx != NULL);
    if (ctx == NULL) {
        flb_free(buffer_dir);
        return;
    }
    flb_service_set(ctx, "flush", "1", "grace", "1", "log_level", "error", NULL);

    in_ffd = flb_input(ctx, (char *) "lib", NULL);
    TEST_CHECK(in_ffd >= 0);
    flb_input_set(ctx, in_ffd, "tag", "test", NULL);

    out_ffd = flb_output(ctx, (char *) "azure_blob", NULL);
    TEST_CHECK(out_ffd >= 0);
    azb_timer_retry_setup(ctx, out_ffd, buffer_dir);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_destroy(ctx);
        flb_free(buffer_dir);
        return;
    }

    push_records(ctx, in_ffd, 30);

    /* Wait for the flush path to buffer at least one chunk: the timer ticks
     * at 1s and each retry attempt takes ~2s, so a blocking sleep in the
     * timer would starve the flush path for 10s+; buffered chunks show the
     * flush path kept running throughout. */
    for (i = 0; i < 100; i++) {
        azb = get_azure_blob_context(ctx);
        if (azb != NULL && mk_list_size(&azb->stream_active->files) > 0) {
            break;
        }
        flb_time_msleep(AZB_TEST_WAIT_STEP_MS);
    }

    azb = get_azure_blob_context(ctx);
    TEST_CHECK(azb != NULL);
    if (azb != NULL) {
        TEST_CHECK(azb->timer_created == FLB_TRUE);
        TEST_CHECK(mk_list_size(&azb->stream_active->files) > 0);
    }

    flb_stop(ctx);
    flb_destroy(ctx);
    flb_free(buffer_dir);
}

/*
 * upload_on_flush=false must keep uploads off the flush path: the buffered
 * file stays open (active) even after it becomes older than upload_timeout.
 */
static void flb_test_upload_on_flush_disabled_defers_upload()
{
    int ret;
    flb_ctx_t *ctx;
    int in_ffd;
    int out_ffd;
    char *buffer_dir;
    struct flb_azure_blob *azb;
    int i;

    buffer_dir = azb_create_buffer_dir("/flb-azb-buffer-no-flush-XXXXXX");
    TEST_CHECK(buffer_dir != NULL);
    if (buffer_dir == NULL) {
        return;
    }

    ctx = flb_create();
    TEST_CHECK(ctx != NULL);
    if (ctx == NULL) {
        flb_free(buffer_dir);
        return;
    }
    flb_service_set(ctx, "flush", "1", "grace", "1", "log_level", "error", NULL);

    in_ffd = flb_input(ctx, (char *) "lib", NULL);
    TEST_CHECK(in_ffd >= 0);
    flb_input_set(ctx, in_ffd, "tag", "test", NULL);

    out_ffd = flb_output(ctx, (char *) "azure_blob", NULL);
    TEST_CHECK(out_ffd >= 0);
    azb_timer_retry_setup(ctx, out_ffd, buffer_dir);
    flb_output_set(ctx, out_ffd, "upload_on_flush", "false", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_destroy(ctx);
        flb_free(buffer_dir);
        return;
    }

    push_records(ctx, in_ffd, 10);

    /* Wait past upload_timeout (2s) plus margin for several timer ticks */
    for (i = 0; i < 10; i++) {
        flb_time_msleep(1000);
        azb = get_azure_blob_context(ctx);
        if (azb != NULL &&
            mk_list_size(&azb->stream_active->files) > 0) {
            break;
        }
    }

    azb = get_azure_blob_context(ctx);
    TEST_CHECK(azb != NULL);
    if (azb != NULL) {
        /* The buffered file must still be around: without upload_on_flush
         * the flush callback never triggers the upload even when the file
         * is past upload_timeout. */
        TEST_CHECK(mk_list_size(&azb->stream_active->files) > 0);
        TEST_CHECK(azb->upload_on_flush == FLB_FALSE);
    }

    flb_stop(ctx);
    flb_destroy(ctx);
    flb_free(buffer_dir);
}

/* Buffer files must go back 'down' (unmapped) after each buffered append,
 * so a disk backlog does not hold memory mappings against process RSS. */
static void flb_test_buffer_files_unmapped_after_append()
{
    int ret;
    flb_ctx_t *ctx;
    int in_ffd;
    int out_ffd;
    char *buffer_dir;
    struct flb_azure_blob *azb;
    struct azure_blob_file *file;
    struct flb_fstore_file *fsf;
    int i;

    buffer_dir = azb_create_buffer_dir("/flb-azb-buffer-unmap-XXXXXX");
    TEST_CHECK(buffer_dir != NULL);
    if (buffer_dir == NULL) {
        return;
    }

    ctx = flb_create();
    TEST_CHECK(ctx != NULL);
    if (ctx == NULL) {
        flb_free(buffer_dir);
        return;
    }
    flb_service_set(ctx, "flush", "1", "grace", "1", "log_level", "error", NULL);

    in_ffd = flb_input(ctx, (char *) "lib", NULL);
    TEST_CHECK(in_ffd >= 0);
    flb_input_set(ctx, in_ffd, "tag", "test", NULL);

    out_ffd = flb_output(ctx, (char *) "azure_blob", NULL);
    TEST_CHECK(out_ffd >= 0);
    azb_timer_retry_setup(ctx, out_ffd, buffer_dir);
    flb_output_set(ctx, out_ffd, "upload_on_flush", "false", NULL);

    ret = flb_start(ctx);
    TEST_CHECK(ret == 0);
    if (ret != 0) {
        flb_destroy(ctx);
        flb_free(buffer_dir);
        return;
    }

    push_records(ctx, in_ffd, 10);

    /* Wait for at least one buffered file to exist */
    for (i = 0; i < 100; i++) {
        azb = get_azure_blob_context(ctx);
        if (azb != NULL && mk_list_size(&azb->stream_active->files) > 0) {
            break;
        }
        flb_time_msleep(AZB_TEST_WAIT_STEP_MS);
    }

    azb = get_azure_blob_context(ctx);
    TEST_CHECK(azb != NULL);
    if (azb != NULL) {
        TEST_CHECK(mk_list_size(&azb->stream_active->files) > 0);
        fsf = mk_list_entry_first(&azb->stream_active->files,
                                  struct flb_fstore_file, _head);
        file = (struct azure_blob_file *) fsf->data;
        TEST_CHECK(file != NULL);
        if (file != NULL) {
            TEST_CHECK(cio_chunk_is_up(fsf->chunk) == CIO_FALSE);
        }
    }

    flb_stop(ctx);
    flb_destroy(ctx);
    flb_free(buffer_dir);
}

TEST_LIST = {
    {"buffer_flush_not_blocked_by_timer", flb_test_buffer_flush_not_blocked_by_timer},
    {"upload_on_flush_disabled_defers_upload", flb_test_upload_on_flush_disabled_defers_upload},
    {"buffer_files_unmapped_after_append", flb_test_buffer_files_unmapped_after_append},
    { 0 }
};
