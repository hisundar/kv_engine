/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#include "connections.h"
#include "runtime.h"
#include "utilities/protocol2text.h"
#include "settings.h"
#include "stats.h"

#include <cJSON.h>
#include <list>
#include <algorithm>
#include <platform/cb_malloc.h>

/*
 * Free list management for connections.
 */
struct connections {
    std::mutex mutex;
    std::list<Connection*> conns;
} connections;


/** Types ********************************************************************/

/** Result of a buffer loan attempt */
enum class BufferLoan {
    Existing,
    Loaned,
    Allocated,
};

/** Function prototypes ******************************************************/

static BufferLoan loan_single_buffer(McbpConnection& c,
                                     std::unique_ptr<cb::Pipe>& thread_buf,
                                     std::unique_ptr<cb::Pipe>& conn_buf);
static void maybe_return_single_buffer(McbpConnection& c,
                                       std::unique_ptr<cb::Pipe>& thread_buf,
                                       std::unique_ptr<cb::Pipe>& conn_buf);
static void conn_destructor(Connection *c);
static Connection *allocate_connection(SOCKET sfd,
                                       event_base *base,
                                       const ListeningPort &interface);

static ListenConnection* allocate_listen_connection(SOCKET sfd,
                                                    event_base* base,
                                                    in_port_t port,
                                                    sa_family_t family,
                                                    const struct interface& interf);

static void release_connection(Connection *c);

/** External functions *******************************************************/
int signal_idle_clients(LIBEVENT_THREAD *me, int bucket_idx, bool logging)
{
    // We've got a situation right now where we're seeing that
    // some of the connections is "stuck". Let's dump all
    // information until we solve the bug.
    logging = true;

    int connected = 0;
    std::lock_guard<std::mutex> lock(connections.mutex);
    for (auto* c : connections.conns) {
        if (c->getThread() == me) {
            ++connected;
            if (bucket_idx == -1 || c->getBucketIndex() == bucket_idx) {
                c->signalIfIdle(logging, me->index);
            }
        }
    }

    return connected;
}

void iterate_thread_connections(LIBEVENT_THREAD* thread,
                                std::function<void(Connection&)> callback) {
    // Deny modifications to the connection map while we're iterating
    // over it
    std::lock_guard<std::mutex> lock(connections.mutex);
    for (auto* c : connections.conns) {
        if (c->getThread() == thread) {
            callback(*c);
        }
    }
}

void destroy_connections(void)
{
    std::lock_guard<std::mutex> lock(connections.mutex);
    /* traverse the list of connections. */
    for (auto* c : connections.conns) {
        conn_destructor(c);
    }
    connections.conns.clear();
}

void close_all_connections(void)
{
    /* traverse the list of connections. */
    {
        std::lock_guard<std::mutex> lock(connections.mutex);
        for (auto* c : connections.conns) {
            if (!c->isSocketClosed()) {
                safe_close(c->getSocketDescriptor());
                c->setSocketDescriptor(INVALID_SOCKET);
            }

            if (c->getRefcount() > 1) {
                auto* mcbp = dynamic_cast<McbpConnection*>(c);
                if (mcbp == nullptr) {
                    abort();
                } else {
                    perform_callbacks(ON_DISCONNECT, NULL, mcbp);
                }
            }
        }
    }

    /*
     * do a second loop, this time wait for all of them to
     * be closed.
     */
    bool done;
    do {
        done = true;
        {
            std::lock_guard<std::mutex> lock(connections.mutex);
            for (auto* c : connections.conns) {
                if (c->getRefcount() > 1) {
                    done = false;
                    break;
                }
            }
        }

        if (!done) {
            usleep(500);
        }
    } while (!done);
}

void run_event_loop(Connection* c, short which) {
    const auto start = ProcessClock::now();
    c->runEventLoop(which);
    const auto stop = ProcessClock::now();

    using namespace std::chrono;
    const auto ns = duration_cast<nanoseconds>(stop - start);
    c->addCpuTime(ns);

    auto* thread = c->getThread();
    if (thread != nullptr) {
        scheduler_info[thread->index].add(ns);
    }

    if (c->shouldDelete()) {
        release_connection(c);
    }
}

ListenConnection* conn_new_server(const SOCKET sfd,
                                  in_port_t parent_port,
                                  sa_family_t family,
                                  const struct interface& interf,
                                  struct event_base* base) {
    auto* c = allocate_listen_connection(sfd, base, parent_port, family, interf);
    if (c == nullptr) {
        return nullptr;
    }
    c->incrementRefcount();

    MEMCACHED_CONN_ALLOCATE(c->getId());
    LOG_DEBUG(c, "<%d server listening on %s", sfd, c->getSockname().c_str());

    stats.total_conns++;
    return c;
}

Connection* conn_new(const SOCKET sfd, in_port_t parent_port,
                     struct event_base* base,
                     LIBEVENT_THREAD* thread) {

    Connection* c;
    {
        std::lock_guard<std::mutex> guard(stats_mutex);
        auto* interface = get_listening_port_instance(parent_port);
        if (interface == nullptr) {
            LOG_WARNING(NULL,
                        "%u: failed to locate server port %u. Disconnecting",
                        (unsigned int)sfd, parent_port);
            return nullptr;
        }

        c = allocate_connection(sfd, base, *interface);
    }

    if (c == nullptr) {
        return nullptr;
    }

    LOG_INFO(nullptr,
             "%u: Accepted new client %s using protocol: %s",
             c->getId(),
             c->getDescription().c_str(),
             to_string(c->getProtocol()).c_str());

    stats.total_conns++;

    c->incrementRefcount();

    associate_initial_bucket(*c);

    c->setThread(thread);
    MEMCACHED_CONN_ALLOCATE(c->getId());

    if (settings.getVerbose() > 1) {
        LOG_DEBUG(c, "<%d new client connection", sfd);
    }

    return c;
}

static void conn_cleanup(McbpConnection& connection) {
    connection.setInternal(false);
    connection.releaseTempAlloc();
    connection.read->clear();
    connection.write->clear();
    /* Return any buffers back to the thread; before we disassociate the
     * connection from the thread. Note we clear DCP status first, so
     * conn_return_buffers() will actually free the buffers.
     */
    connection.setDCP(false);
    conn_return_buffers(&connection);
    connection.getCookieObject().reset();
    connection.setEngineStorage(nullptr);

    connection.setThread(nullptr);
    cb_assert(connection.getNext() == nullptr);
    connection.setSocketDescriptor(INVALID_SOCKET);
    connection.setStart(ProcessClock::time_point());
    connection.disableSSL();
}

void conn_close(McbpConnection& connection) {
    if (!connection.isSocketClosed()) {
        throw std::logic_error("conn_cleanup: socketDescriptor must be closed");
    }
    if (connection.getState() != McbpStateMachine::State::immediate_close) {
        throw std::logic_error("conn_cleanup: Connection:state (which is " +
                               std::string(connection.getStateName()) +
                               ") must be conn_immediate_close");
    }

    auto thread = connection.getThread();
    if (thread == nullptr) {
        throw std::logic_error("conn_close: unable to obtain non-NULL thread from connection");
    }
    /* remove from pending-io list */
    if (settings.getVerbose() > 1 &&
        list_contains(thread->pending_io, &connection)) {
        LOG_WARNING(
                &connection,
                "Current connection was in the pending-io list.. Nuking it");
    }
    thread->pending_io = list_remove(thread->pending_io, &connection);

    conn_cleanup(connection);

    if (connection.getThread() != nullptr) {
        throw std::logic_error("conn_close: failed to disassociate connection from thread");
    }
    connection.setState(McbpStateMachine::State::destroyed);
}

ListeningPort *get_listening_port_instance(const in_port_t port) {
    for (auto &instance : stats.listening_ports) {
        if (instance.port == port) {
            return &instance;
        }
    }

    return nullptr;
}

void connection_stats(ADD_STAT add_stats, const void* cookie, const int64_t fd) {
    std::lock_guard<std::mutex> lock(connections.mutex);
    for (auto *c : connections.conns) {
        if (c->getSocketDescriptor() == fd || fd == -1) {
            auto stats = c->toJSON();
            // no key, JSON value contains all properties of the connection.
            auto stats_str = to_string(stats, false);
            add_stats(nullptr, 0, stats_str.data(), uint32_t(stats_str.size()),
                      cookie);
        }
    }
}

#ifndef WIN32
/**
 * NOTE: This is <b>not</b> intended to be called during normal situation,
 * but in the case where we've been exhausting all connections to memcached
 * we need a way to be able to dump the connection states in order to search
 * for a bug.
 */
void dump_connection_stat_signal_handler(evutil_socket_t, short, void *) {
    std::lock_guard<std::mutex> lock(connections.mutex);
    for (auto *c : connections.conns) {
        try {
            auto json = c->toJSON();
            auto info = to_string(json, false);
            LOG_NOTICE(c, "Connection: %s", info.c_str());
        } catch (const std::bad_alloc&) {
            LOG_NOTICE(c, "Failed to allocate memory to dump info for %u",
                       c->getId());
        }
    }
}
#endif


void conn_loan_buffers(Connection *connection) {
    auto *c = dynamic_cast<McbpConnection*>(connection);
    if (c == nullptr) {
        return;
    }

    auto* ts = get_thread_stats(c);
    switch (loan_single_buffer(*c, c->getThread()->read, c->read)) {
    case BufferLoan::Existing:
        ts->rbufs_existing++;
        break;
    case BufferLoan::Loaned:
        ts->rbufs_loaned++;
        break;
    case BufferLoan::Allocated:
        ts->rbufs_allocated++;
        break;
    }

    switch (loan_single_buffer(*c, c->getThread()->write, c->write)) {
    case BufferLoan::Existing:
        ts->wbufs_existing++;
        break;
    case BufferLoan::Loaned:
        ts->wbufs_loaned++;
        break;
    case BufferLoan::Allocated:
        ts->wbufs_allocated++;
        break;
    }
}

void conn_return_buffers(Connection *connection) {
    auto *c = dynamic_cast<McbpConnection*>(connection);
    if (c == nullptr) {
        return;
    }

    auto thread = c->getThread();

    if (thread == nullptr) {
        // Connection already cleaned up - nothing to do.
        return;
    }

    if (c->isDCP()) {
        // DCP work differently - let them keep their buffers once allocated.
        return;
    }

    maybe_return_single_buffer(*c, thread->read, c->read);
    maybe_return_single_buffer(*c, thread->write, c->write);
}

/** Internal functions *******************************************************/

/**
 * Destructor for all connection objects. Release all allocated resources.
 */
static void conn_destructor(Connection *c) {
    delete c;
    stats.conn_structs--;
}

/** Allocate a connection, creating memory and adding it to the conections
 *  list. Returns a pointer to the newly-allocated connection if successful,
 *  else NULL.
 */
static Connection *allocate_connection(SOCKET sfd,
                                       event_base *base,
                                       const ListeningPort &interface) {
    Connection *ret = nullptr;

    try {
        ret = new McbpConnection(sfd, base, interface);
        std::lock_guard<std::mutex> lock(connections.mutex);
        connections.conns.push_back(ret);
        stats.conn_structs++;
        return ret;
    } catch (const std::bad_alloc&) {
        LOG_WARNING(NULL, "Failed to allocate memory for connection");
    } catch (const std::exception& error) {
        LOG_WARNING(NULL, "Failed to create connection: %s", error.what());
    } catch (...) {
        LOG_WARNING(NULL, "Failed to create connection");
    }

    delete ret;
    return NULL;
}

static ListenConnection* allocate_listen_connection(SOCKET sfd,
                                                    event_base* base,
                                                    in_port_t port,
                                                    sa_family_t family,
                                                    const struct interface& interf) {
    ListenConnection *ret = nullptr;

    try {
        ret = new ListenConnection(sfd, base, port, family, interf);
        std::lock_guard<std::mutex> lock(connections.mutex);
        connections.conns.push_back(ret);
        stats.conn_structs++;
        return ret;
    } catch (const std::bad_alloc&) {
        LOG_WARNING(NULL, "Failed to allocate memory for listen connection");
    } catch (const std::exception& error) {
        LOG_WARNING(NULL, "Failed to create connection: %s", error.what());
    } catch (...) {
        LOG_WARNING(NULL, "Failed to create connection");
    }

    delete ret;
    return NULL;
}

/** Release a connection; removing it from the connection list management
 *  and freeing the Connection object.
 */
static void release_connection(Connection *c) {
    {
        std::lock_guard<std::mutex> lock(connections.mutex);
        auto iter = std::find(connections.conns.begin(), connections.conns.end(), c);
        // I should assert
        cb_assert(iter != connections.conns.end());
        connections.conns.erase(iter);
    }

    // Finally free it
    conn_destructor(c);
}

/**
 * If the connection doesn't already have a populated conn_buff, ensure that
 * it does by either loaning out the threads, or allocating a new one if
 * necessary.
 */
static BufferLoan loan_single_buffer(McbpConnection& c,
                                     std::unique_ptr<cb::Pipe>& thread_buf,
                                     std::unique_ptr<cb::Pipe>& conn_buf) {
    /* Already have a (partial) buffer - nothing to do. */
    if (conn_buf) {
        return BufferLoan::Existing;
    }

    // If the thread has a buffer, let's loan that to the connection
    if (thread_buf) {
        thread_buf.swap(conn_buf);
        return BufferLoan::Loaned;
    }

    // Need to allocate a new buffer
    try {
        conn_buf = std::make_unique<cb::Pipe>(DATA_BUFFER_SIZE);
    } catch (const std::bad_alloc&) {
        // Unable to alloc a buffer for the thread. Not much we can do here
        // other than terminate the current connection.
        if (settings.getVerbose()) {
            LOG_WARNING(&c,
                        "%u: Failed to allocate new network buffer.. closing"
                        " connection",
                        c.getId());
        }
        c.setState(McbpStateMachine::State::closing);
        return BufferLoan::Existing;
    }

    return BufferLoan::Allocated;
}

static void maybe_return_single_buffer(McbpConnection& c,
                                       std::unique_ptr<cb::Pipe>& thread_buf,
                                       std::unique_ptr<cb::Pipe>& conn_buf) {
    if (conn_buf && conn_buf->empty()) {
        // Buffer clean, dispose of it
        if (thread_buf) {
            // Already got a thread buffer.. release this one
            conn_buf.reset();
        } else {
            conn_buf.swap(thread_buf);
        }
    }
}

ENGINE_ERROR_CODE apply_connection_trace_mask(const std::string& connid,
                                              const std::string& mask) {
    uint32_t id;
    try {
        id = static_cast<uint32_t>(std::stoi(connid));
    } catch (...) {
        return ENGINE_EINVAL;
    }

    bool enable = mask != "0";
    bool found = false;

    {
        // Lock the connection array to avoid race conditions with
        // connections being added / removed / destroyed
        std::unique_lock<std::mutex> lock(connections.mutex);
        for (auto* c : connections.conns) {
            if (c->getId() == id) {
                c->setTraceEnabled(enable);
                found = true;
            }
        }
    }

    if (found) {
        const char *message = enable ? "Enabled" : "Disabled";
        LOG_NOTICE(nullptr, "%s trace for %u", message, id);
        return ENGINE_SUCCESS;
    }

    return ENGINE_KEY_ENOENT;
}
