/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
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
#pragma once

#include "config.h"

#include "rbac.h"

#include <cJSON.h>
#include <cbsasl/cbsasl.h>
#include <string>

struct LIBEVENT_THREAD;

/**
 * The structure representing a connection in memcached.
 */
class Connection {
public:
    enum class Priority : uint8_t {
        High,
        Medium,
        Low
    };

    virtual ~Connection();

    Connection(const Connection&) = delete;

    /**
     * Return an identifier for this connection. To be backwards compatible
     * this is the socket filedescriptor (or the socket handle casted to an
     * unsigned integer on windows).
     */
    uint32_t getId() const {
        return uint32_t(socketDescriptor);
    }

    /**
     *  Get the socket descriptor used by this connection.
     */
    SOCKET getSocketDescriptor() const {
        return socketDescriptor;
    }

    /**
     * Set the socket descriptor used by this connection
     */
    void setSocketDescriptor(SOCKET sfd) {
        Connection::socketDescriptor = sfd;
    }

    bool isSocketClosed() const {
        return socketDescriptor == INVALID_SOCKET;
    }

    /**
     * Resolve the name of the local socket and the peer for the connected
     * socket.
     * @param listening True if the local socket is a listening socket.
     */
    void resolveConnectionName(bool listening);

    const std::string& getPeername() const {
        return peername;
    }

    const std::string& getSockname() const {
        return sockname;
    }

    /**
     * Returns a descriptive name for the connection, of the form:
     *   "[peer_name - local_name ]"
     *(A) is appended to the string for admin connections.
     */
    std::string getDescription() const;

    /**
     * Tell the connection to initiate it's shutdown logic
     */
    virtual void initateShutdown() {
        throw std::runtime_error("Not implemented");
    }

    /**
     * Signal a connection if it's idle
     *
     * @param logbusy set to true if you want to log the connection details
     *                if the connection isn't idle
     * @param workerthead the id of the workerthread (for logging purposes)
     */
    virtual void signalIfIdle(bool logbusy, int workerthread) {

    }

    /**
     * Terminate the eventloop for the current event base. This method doesn't
     * really fit as a member for the class, but I don't want clients to access
     * the libevent details from outside the class (so I didn't want to make
     * a "getEventBase()" method.
     */
    void eventBaseLoopbreak() {
        event_base_loopbreak(base);
    }


    /** Is the connection authorized with admin privileges? */
    bool isAdmin() const {
        return admin;
    }

    void setAdmin(bool admin) {
        Connection::admin = admin;
    }

    bool isAuthenticated() const {
        return authenticated;
    }

    void setAuthenticated(bool authenticated) {
        Connection::authenticated = authenticated;

        static const char unknown[] = "unknown";
        const void* unm = unknown;

        if (authenticated) {
            if (cbsasl_getprop(sasl_conn, CBSASL_USERNAME, &unm) != CBSASL_OK) {
                unm = unknown;
            }
        }

        username.assign(reinterpret_cast<const char*>(unm));
    }


    const Priority& getPriority() const {
        return priority;
    }

    virtual void setPriority(const Priority& priority) {
        Connection::priority = priority;
    }

    virtual const Protocol getProtocol() const = 0;

    /**
     * Create a cJSON representation of the members of the connection
     * Caller is responsible for freeing the result with cJSON_Delete().
     */
    virtual cJSON* toJSON() const;

    /**
     * Enable or disable TCP NoDelay on the underlying socket
     *
     * @return true on success, false otherwise
     */
    bool setTcpNoDelay(bool enable);

    /**
     * Get the username this connection is authenticated as
     *
     * NOTE: the return value should not be returned by the client
     */
    const char* getUsername() const {
        return username.c_str();
    }

    cbsasl_conn_t* getSaslConn() const {
        return sasl_conn;
    }

    void setSaslConn(cbsasl_conn_t* sasl_conn) {
        Connection::sasl_conn = sasl_conn;
    }

    /**
     * Get the current reference count
     */
    uint8_t getRefcount() const {
        return refcount;
    }

    void incrementRefcount() {
        ++refcount;
    }

    void decrementRefcount() {
        --refcount;
    }

    Connection* getNext() const {
        return next;
    }

    void setNext(Connection* next) {
        Connection::next = next;
    }

    LIBEVENT_THREAD* getThread() const {
        return thread.load(std::memory_order_relaxed);
    }

    void setThread(LIBEVENT_THREAD* thread) {
        Connection::thread.store(thread,
                                 std::memory_order::memory_order_relaxed);
    }

    /**
     * Update the authentication context to operate on behalf of a given
     * role
     */
    AuthResult assumeRole(const std::string& role) {
        return auth_assume_role(auth_context, role.c_str());
    }

    /**
     * Update the authentication context to operate as the authenticated
     * user rather than the current role
     */
    AuthResult dropRole() {
        return auth_drop_role(auth_context);
    }

    /**
     * @todo this should be pushed down to MCBP, doesn't apply to everyone else
     */
    virtual bool isPipeConnection() {
        return false;
    }

    /**
     * @todo this should be pushed down to MCBP, doesn't apply to everyone else
     */
    virtual bool isSupportsDatatype() const {
        return true;
    }

    /**
     * @todo this should be pushed down to MCBP, doesn't apply to everyone else
     */
    virtual bool isSupportsMutationExtras() const {
        return true;
    }

    in_port_t getParentPort() const {
        return parent_port;
    }

    void setParentPort(in_port_t parent_port) {
        Connection::parent_port = parent_port;
    }

    virtual bool isTAP() const {
        return false;
    }

    virtual bool isDCP() const {
        return false;
    }

    /**
     * Set the authentication context to be used by this connection.
     *
     * The connection object takes the ownership of the pointer and is
     * responsible for releasing the memory.
     */
    void setAuthContext(AuthContext* auth_context) {
        if (Connection::auth_context != nullptr) {
            // Delete the previously allocated auth object
            auth_destroy(Connection::auth_context);
        }
        Connection::auth_context = auth_context;
    }

    /**
     * Check if the client is allowed to execute the specified opcode
     *
     * @param opcode the opcode in the memcached binary protocol
     */
    AuthResult checkAccess(uint8_t opcode) const {
        return auth_check_access(auth_context, opcode);
    }

    /**
     * Check if the client is allowed to execute the specified opcode
     *
     * @param opcode the opcode in the memcached binary protocol
     */
    AuthResult checkAccess(const Greenstack::Opcode& opcode) const {
        return auth_check_access(auth_context, opcode);
    }


    int getBucketIndex() const {
        return bucketIndex;
    }

    void setBucketIndex(int bucketIndex) {
        Connection::bucketIndex = bucketIndex;
    }

    ENGINE_HANDLE_V1* getBucketEngine() const {
        return bucketEngine;
    };

    ENGINE_HANDLE* getBucketEngineAsV0() const {
        return reinterpret_cast<ENGINE_HANDLE*>(bucketEngine);
    }

    void setBucketEngine(ENGINE_HANDLE_V1* bucketEngine) {
        Connection::bucketEngine = bucketEngine;
    };

    void* getEngineStorage() const {
        return engine_storage;
    }

    void setEngineStorage(void* engine_storage) {
        Connection::engine_storage = engine_storage;
    }

    virtual bool shouldDelete() {
        return false;
    }

    virtual void runEventLoop(short which) = 0;

protected:
    Connection(SOCKET sfd, event_base* b);

    Connection(SOCKET sfd, event_base* b,
               const struct listening_port& interface);

    /**
     * The actual socket descriptor used by this connection
     */
    SOCKET socketDescriptor;

    /**
     * The event base this connection is bound to
     */
    event_base *base;

    /**
     * The SASL object used to do sasl authentication
     */
    cbsasl_conn_t* sasl_conn;

    /** Is the connection set up with admin privileges */
    bool admin;

    /** Is the connection authenticated or not */
    bool authenticated;

    /** The username authenticated as */
    std::string username;


    /** Is tcp nodelay enabled or not? */
    bool nodelay;

    /** number of references to the object */
    uint8_t refcount;

    /**
     * Pointer to engine-specific data which the engine has requested the server
     * to persist for the life of the connection.
     * See SERVER_COOKIE_API::{get,store}_engine_specific()
     */
    void* engine_storage;

    /* Used for generating a list of Connection structures */
    Connection* next;

    /** Pointer to the thread object serving this connection */
    std::atomic<LIBEVENT_THREAD*> thread;

    /** Listening port that creates this connection instance */
    in_port_t parent_port;

    /**
     * The authentication context in use by this connection
     */
    AuthContext* auth_context;

    /**
     * The index of the connected bucket
     */
    int bucketIndex;

    /**
     * The engine interface for the connected bucket
     */
    ENGINE_HANDLE_V1* bucketEngine;

    /** Name of the peer if known */
    std::string peername;

    /** Name of the local socket if known */
    std::string sockname;

    /** The connections priority */
    Priority priority;
};

/**
 * Convert a priority to a textual representation
 */
const char* to_string(const Connection::Priority& priority);
