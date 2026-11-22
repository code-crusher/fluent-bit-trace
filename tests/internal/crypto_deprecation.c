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

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_crypto.h>
#include <fluent-bit/flb_hash.h>
#include <fluent-bit/flb_hmac.h>

#include <stdio.h>
#include <string.h>

#include "flb_tests_internal.h"
#include "../include/flb_tests_stderr.h"

#define MD5_ABC_HEX          "900150983cd24fb0d6963f7d28e17f72"
#define HMAC_MD5_JEFE_HEX    "750c783e6ab0b503eaa86e310a5db738"

static void hexlify(const unsigned char *bytes, size_t length, char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t index;

    for (index = 0; index < length; index++) {
        out[index * 2] = hex[bytes[index] >> 4];
        out[index * 2 + 1] = hex[bytes[index] & 0xf];
    }
    out[length * 2] = '\0';
}

static void test_md5_hash_warns_once_and_keeps_working(void)
{
    int ret;
    char hex_digest[33];
    char log_buffer[4096];
    unsigned char digest[16];
    struct flb_test_stderr_capture capture;

    flb_crypto_deprecation_warnings_reset();

    TEST_ASSERT(flb_test_stderr_capture_start(&capture) == 0);

    ret = flb_hash_simple(FLB_HASH_MD5, (unsigned char *) "abc", 3,
                          digest, sizeof(digest));
    TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);

    ret = flb_hash_simple(FLB_HASH_MD5, (unsigned char *) "abc", 3,
                          digest, sizeof(digest));
    TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);

    flb_test_stderr_capture_stop(&capture, log_buffer, sizeof(log_buffer));

    /* Backward compatibility: MD5 digests are still computed as before */
    hexlify(digest, sizeof(digest), hex_digest);
    TEST_CHECK(strcmp(hex_digest, MD5_ABC_HEX) == 0);

    /* The deprecation notice is emitted once per process, not per call */
    TEST_CHECK(flb_test_count_substring(log_buffer, "MD5 is deprecated") == 1);
    TEST_MSG("captured log: %s", log_buffer);
}

static void test_md5_hmac_warns_once_and_keeps_working(void)
{
    int ret;
    char hex_signature[33];
    char log_buffer[4096];
    unsigned char signature[16];
    struct flb_hmac hmac;
    struct flb_test_stderr_capture capture;

    flb_crypto_deprecation_warnings_reset();

    TEST_ASSERT(flb_test_stderr_capture_start(&capture) == 0);

    ret = flb_hmac_init(&hmac, FLB_HASH_MD5,
                        (unsigned char *) "Jefe", 4);
    TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);
    if (ret == FLB_CRYPTO_SUCCESS) {
        ret = flb_hmac_update(&hmac,
                              (unsigned char *) "what do ya want for nothing?", 28);
        TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);

        ret = flb_hmac_finalize(&hmac, signature, sizeof(signature));
        TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);

        flb_hmac_cleanup(&hmac);
    }

    /* A second initialization must not emit another warning */
    ret = flb_hmac_init(&hmac, FLB_HASH_MD5,
                        (unsigned char *) "Jefe", 4);
    TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);
    if (ret == FLB_CRYPTO_SUCCESS) {
        flb_hmac_cleanup(&hmac);
    }

    flb_test_stderr_capture_stop(&capture, log_buffer, sizeof(log_buffer));

    /* Backward compatibility: HMAC-MD5 signatures are still computed as before */
    hexlify(signature, sizeof(signature), hex_signature);
    TEST_CHECK(strcmp(hex_signature, HMAC_MD5_JEFE_HEX) == 0);

    TEST_CHECK(flb_test_count_substring(log_buffer, "MD5 is deprecated") == 1);
    TEST_MSG("captured log: %s", log_buffer);
}

static void test_sha1_warning_is_independent_from_md5(void)
{
    char log_buffer[4096];
    struct flb_test_stderr_capture capture;

    flb_crypto_deprecation_warnings_reset();

    TEST_ASSERT(flb_test_stderr_capture_start(&capture) == 0);

    flb_crypto_warn_deprecated_sha1();
    flb_crypto_warn_deprecated_sha1();
    flb_crypto_warn_deprecated_md5();

    flb_test_stderr_capture_stop(&capture, log_buffer, sizeof(log_buffer));

    TEST_CHECK(flb_test_count_substring(log_buffer, "SHA-1 is deprecated") == 1);
    TEST_CHECK(flb_test_count_substring(log_buffer, "MD5 is deprecated") == 1);
    TEST_MSG("captured log: %s", log_buffer);
}

static void test_reset_reenables_deprecation_warning(void)
{
    char log_buffer[4096];
    struct flb_test_stderr_capture capture;

    flb_crypto_deprecation_warnings_reset();

    TEST_ASSERT(flb_test_stderr_capture_start(&capture) == 0);

    flb_crypto_warn_deprecated_md5();
    flb_crypto_deprecation_warnings_reset();
    flb_crypto_warn_deprecated_md5();

    flb_test_stderr_capture_stop(&capture, log_buffer, sizeof(log_buffer));

    TEST_CHECK(flb_test_count_substring(log_buffer, "MD5 is deprecated") == 2);
    TEST_MSG("captured log: %s", log_buffer);
}

static void test_sha256_does_not_warn(void)
{
    int ret;
    char log_buffer[4096];
    unsigned char digest[64];
    struct flb_test_stderr_capture capture;

    flb_crypto_deprecation_warnings_reset();

    TEST_ASSERT(flb_test_stderr_capture_start(&capture) == 0);

    ret = flb_hash_simple(FLB_HASH_SHA256, (unsigned char *) "abc", 3,
                          digest, sizeof(digest));
    TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);

    ret = flb_hash_simple(FLB_HASH_SHA512, (unsigned char *) "abc", 3,
                          digest, sizeof(digest));
    TEST_CHECK(ret == FLB_CRYPTO_SUCCESS);

    flb_test_stderr_capture_stop(&capture, log_buffer, sizeof(log_buffer));

    TEST_CHECK(strstr(log_buffer, "deprecated") == NULL);
    TEST_MSG("captured log: %s", log_buffer);
}

TEST_LIST = {
    {"md5_hash_warns_once_and_keeps_working",
     test_md5_hash_warns_once_and_keeps_working},
    {"md5_hmac_warns_once_and_keeps_working",
     test_md5_hmac_warns_once_and_keeps_working},
    {"sha1_warning_is_independent_from_md5",
     test_sha1_warning_is_independent_from_md5},
    {"reset_reenables_deprecation_warning",
     test_reset_reenables_deprecation_warning},
    {"sha256_does_not_warn", test_sha256_does_not_warn},
    {0}
};
