/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "flush_command_context.h"

#include <daemon/mcbp.h>
#include <daemon/mcaudit.h>

ENGINE_ERROR_CODE FlushCommandContext::flushing() {
    state = State::Done;
    return connection.getBucketEngine()->flush(connection.getBucketEngineAsV0(),
                                               static_cast<void*>(&cookie));
}

void FlushCommandContext::done() {
    audit_bucket_flush(&connection, connection.getBucket().name);
    get_thread_stats(&connection)->cmd_flush++;
    cookie.sendResponse(cb::mcbp::Status::Success);
}
