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

#ifndef FLB_OUT_KINESIS_API
#define FLB_OUT_KINESIS_API

#define PUT_RECORDS_PAYLOAD_SIZE         10485760
#define MAX_EVENTS_PER_PUT               500

/*
 * Default and maximum record sizes, before base64 encoding. Kinesis limits a
 * record (data blob plus partition key) to 1 MiB by default, or up to 10 MiB
 * when the stream is configured to support large records. Both values reserve
 * 20 bytes for the partition key.
 */
#define MAX_EVENT_SIZE                   1048556  /* 1 MiB - 20 bytes */
#define MAX_EVENT_SIZE_LIMIT             10485740 /* 10 MiB - 20 bytes */

/* string form of the default record size, used by the config map */
#define KINESIS_STRINGIFY_(x)            #x
#define KINESIS_STRINGIFY(x)             KINESIS_STRINGIFY_(x)
#define MAX_EVENT_SIZE_DEFAULT           KINESIS_STRINGIFY(MAX_EVENT_SIZE)

/* number of bytes needed to base64 encode a buffer of 'size' bytes */
#define MAX_B64_SIZE(size)               ((((size) + 2) / 3) * 4)

/* number of characters needed to 'start' a PutRecords payload */
#define PUT_RECORDS_HEADER_LEN      30
/* number of characters needed per record in a PutRecords payload */
#define PUT_RECORDS_PER_RECORD_LEN  48
/* number of characters needed to 'end' a PutRecords payload */
#define PUT_RECORDS_FOOTER_LEN      4

/* fixed overhead of a PutRecords payload carrying a single record */
#define PUT_RECORDS_SINGLE_OVERHEAD  (PUT_RECORDS_HEADER_LEN + \
                                      PUT_RECORDS_PER_RECORD_LEN + \
                                      PUT_RECORDS_FOOTER_LEN)

#include "kinesis.h"

void kinesis_flush_destroy(struct flush *buf);

int process_and_send_to_kinesis(struct flb_kinesis *ctx, struct flush *buf,
                                const char *data, size_t bytes,
                                struct flb_config *config);

int put_records(struct flb_kinesis *ctx, struct flush *buf,
                     size_t payload_size, int num_records);

#endif
