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

#include <daemon/mcbp.h>
#include <mcbp/mcbp.h>
#include "dcp_add_failover_log.h"
#include "executors.h"

void dcp_get_failover_log_executor(Cookie& cookie) {
    ENGINE_ERROR_CODE ret = cookie.getAiostat();
    cookie.setAiostat(ENGINE_SUCCESS);
    cookie.setEwouldblock(false);

    auto& connection = cookie.getConnection();
    if (ret == ENGINE_SUCCESS) {
        auto& header = cookie.getHeader().getRequest();
        ret = connection.getBucketEngine()->dcp.get_failover_log(
                connection.getBucketEngineAsV0(),
                &cookie,
                header.getOpaque(),
                header.getVBucket(),
                add_failover_log);
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        if (cookie.getDynamicBuffer().getRoot() != nullptr) {
            cookie.sendDynamicBuffer();
        } else {
            cookie.sendResponse(cb::mcbp::Status::Success);
        }
        break;

    case ENGINE_DISCONNECT:
        connection.setState(McbpStateMachine::State::closing);
        break;

    case ENGINE_EWOULDBLOCK:
        cookie.setEwouldblock(true);
        break;

    default:
        cookie.sendResponse(cb::engine_errc(ret));
    }
}