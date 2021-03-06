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
#include "dcp_deletion.h"
#include "engine_wrapper.h"
#include "utilities.h"
#include "../../mcbp.h"

void dcp_deletion_executor(Cookie& cookie) {
    auto packet = cookie.getPacket(Cookie::PacketContent::Full);
    const auto* req =
            reinterpret_cast<const protocol_binary_request_dcp_deletion*>(
                    packet.data());

    auto& connection = cookie.getConnection();

    ENGINE_ERROR_CODE ret = cookie.getAiostat();
    cookie.setAiostat(ENGINE_SUCCESS);
    cookie.setEwouldblock(false);

    if (ret == ENGINE_SUCCESS) {
        // Collection aware DCP will be sending the collection_len field, so
        // only read for collection-aware DCP
        const auto body_offset =
                protocol_binary_request_dcp_deletion::getHeaderLength(
                        connection.isDcpCollectionAware());

        const uint16_t nkey = ntohs(req->message.header.request.keylen);
        const DocKey key{req->bytes + body_offset,
                         nkey,
                         connection.getDocNamespaceForDcpMessage(
                                 req->message.body.collection_len)};
        const auto opaque = req->message.header.request.opaque;
        const auto datatype = req->message.header.request.datatype;
        const uint64_t cas = ntohll(req->message.header.request.cas);
        const uint16_t vbucket = ntohs(req->message.header.request.vbucket);
        const uint64_t by_seqno = ntohll(req->message.body.by_seqno);
        const uint64_t rev_seqno = ntohll(req->message.body.rev_seqno);
        const uint16_t nmeta = ntohs(req->message.body.nmeta);
        const uint32_t valuelen = ntohl(req->message.header.request.bodylen) -
                                  nkey - req->message.header.request.extlen -
                                  nmeta;
        cb::const_byte_buffer value{req->bytes + body_offset + nkey, valuelen};
        cb::const_byte_buffer meta{value.buf + valuelen, nmeta};
        uint32_t priv_bytes = 0;
        if (mcbp::datatype::is_xattr(datatype)) {
            priv_bytes = valuelen;
        }

        if (priv_bytes > COUCHBASE_MAX_ITEM_PRIVILEGED_BYTES) {
            ret = ENGINE_E2BIG;
        } else {
            ret = connection.getBucketEngine()->dcp.deletion(
                    connection.getBucketEngineAsV0(),
                    &cookie,
                    opaque,
                    key,
                    value,
                    priv_bytes,
                    datatype,
                    cas,
                    vbucket,
                    by_seqno,
                    rev_seqno,
                    meta);
        }
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        connection.setState(McbpStateMachine::State::new_cmd);
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

ENGINE_ERROR_CODE dcp_message_deletion(const void* void_cookie,
                                       uint32_t opaque,
                                       item* it,
                                       uint16_t vbucket,
                                       uint64_t by_seqno,
                                       uint64_t rev_seqno,
                                       const void* meta,
                                       uint16_t nmeta,
                                       uint8_t collection_len) {
    if (void_cookie == nullptr) {
        throw std::invalid_argument(
                "dcp_message_deletion: void_cookie can't be nullptr");
    }
    const auto& ccookie = *static_cast<const Cookie*>(void_cookie);
    auto& cookie = const_cast<Cookie&>(ccookie);
    auto* c = &cookie.getConnection();

    // Use a unique_ptr to make sure we release the item in all error paths
    cb::unique_item_ptr item(it, cb::ItemDeleter{c->getBucketEngineAsV0()});

    item_info info;
    if (!bucket_get_item_info(cookie, it, &info)) {
        LOG_WARNING(c, "%u: dcp_message_deletion: Failed to get item info",
                    c->getId());
        return ENGINE_FAILED;
    }

    if (!c->reserveItem(it)) {
        LOG_WARNING(c, "%u: dcp_message_deletion: Failed to grow item array",
                    c->getId());
        return ENGINE_FAILED;
    }

    // we've reserved the item, and it'll be released when we're done sending
    // the item.
    item.release();

    protocol_binary_request_dcp_deletion packet(c->isDcpCollectionAware(),
                                                opaque,
                                                vbucket,
                                                info.cas,
                                                info.nkey,
                                                info.nbytes,
                                                info.datatype,
                                                by_seqno,
                                                rev_seqno,
                                                nmeta,
                                                collection_len);

    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_DELETION;

    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
    c->write->produce([&c, &packet, &info, &meta, &nmeta, &ret](
                              cb::byte_buffer buffer) -> size_t {

        const size_t packetlen =
                protocol_binary_request_dcp_deletion::getHeaderLength(
                        c->isDcpCollectionAware());

        if (buffer.size() < (packetlen + nmeta)) {
            ret = ENGINE_E2BIG;
            return 0;
        }

        std::copy(packet.bytes, packet.bytes + packetlen, buffer.begin());

        if (nmeta > 0) {
            std::copy(static_cast<const uint8_t*>(meta),
                      static_cast<const uint8_t*>(meta) + nmeta,
                      buffer.data() + packetlen);
        }

        // Add the header
        c->addIov(buffer.data(), packetlen);

        // Add the key
        c->addIov(info.key, info.nkey);

        // Add the optional payload (xattr)
        if (info.nbytes > 0) {
            c->addIov(info.value[0].iov_base, info.nbytes);
        }

        // Add the optional meta section
        if (nmeta > 0) {
            c->addIov(buffer.data() + packetlen, nmeta);
        }

        return packetlen + nmeta;
    });

    return ret;
}
