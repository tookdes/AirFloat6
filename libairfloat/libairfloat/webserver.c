//
//  webserver.c
//  AirFloat
//
//  Copyright (c) 2013, Kristian Trenskow All rights reserved.
//
//  Redistribution and use in source and binary forms, with or
//  without modification, are permitted provided that the following
//  conditions are met:
//
//  Redistributions of source code must retain the above copyright
//  notice, this list of conditions and the following disclaimer.
//  Redistributions in binary form must reproduce the above
//  copyright notice, this list of conditions and the following
//  disclaimer in the documentation and/or other materials provided
//  with the distribution. THIS SOFTWARE IS PROVIDED BY THE
//  COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
//  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
//  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "log.h"
#include "mutex.h"
#include "sockaddr.h"
#include "socket.h"
#include "webserverconnection.h"
#include "webserver.h"

struct web_server_connection_t {
    web_server_connection_p web_connection;
    socket_p socket;
};

struct web_server_t {
    socket_p socket_ipv4;
    socket_p socket_ipv6;
    sockaddr_type socket_types;
    bool is_running;
    web_server_accept_callback accept_callback;
    void* accept_callback_ctx;
    struct web_server_connection_t* connections;
    uint32_t connection_count;
    mutex_p mutex;
};

void _web_server_socket_closed(socket_p socket, void* ctx) {
    
    struct web_server_t* ws = (struct web_server_t*)ctx;
    if (ws == NULL || socket == NULL)
        return;
    
    web_server_connection_p web_connection = NULL;
    bool found = false;
    
    mutex_lock(ws->mutex);
    for (uint32_t i = 0 ; i < ws->connection_count ; i++) {
        if (ws->connections[i].socket == socket) {
            web_connection = ws->connections[i].web_connection;
            for (uint32_t x = i + 1 ; x < ws->connection_count ; x++)
                ws->connections[x - 1] = ws->connections[x];
            ws->connection_count--;
            found = true;
            break;
        }
    }
    mutex_unlock(ws->mutex);
    
    if (found) {
        web_server_connection_destroy(web_connection);
        socket_destroy(socket);
    }
}

struct web_server_t* web_server_create(sockaddr_type socket_types) {
    
    struct web_server_t* ws = (struct web_server_t*)malloc(sizeof(struct web_server_t));
    if (ws == NULL)
        return NULL;
    bzero(ws, sizeof(struct web_server_t));
    
    ws->socket_types = socket_types;
    ws->mutex = mutex_create();
    if (ws->mutex == NULL) {
        free(ws);
        return NULL;
    }
    
    return ws;
}

void web_server_destroy(struct web_server_t* ws) {
    
    if (ws == NULL)
        return;
    
    web_server_stop(ws);
    mutex_destroy(ws->mutex);
    free(ws);
}

socket_p _web_server_bind(struct web_server_t* ws, uint16_t port, sockaddr_type socket_type) {
    
    if (ws == NULL || (ws->socket_types & socket_type) == 0)
        return NULL;
    
    socket_p socket = socket_create("Web server", false);
    if (socket == NULL)
        return NULL;
    
    struct sockaddr* end_point = sockaddr_create(NULL, port, socket_type, 0);
    if (end_point == NULL) {
        socket_destroy(socket);
        return NULL;
    }
    
    bool ret = socket_bind(socket, end_point);
    sockaddr_destroy(end_point);
    
    if (!ret) {
        socket_destroy(socket);
        return NULL;
    }
    
    return socket;
}

bool _web_server_socket_accept_callback(socket_p socket, socket_p new_socket, void* ctx) {
    
    struct web_server_t* ws = (struct web_server_t*)ctx;
    if (ws == NULL || new_socket == NULL)
        return false;
    
    web_server_connection_p new_web_connection = web_server_connection_create(new_socket, ws);
    if (new_web_connection == NULL)
        return false;
    
    web_server_accept_callback accept_callback = NULL;
    void* accept_callback_ctx = NULL;
    
    mutex_lock(ws->mutex);
    accept_callback = ws->accept_callback;
    accept_callback_ctx = ws->accept_callback_ctx;
    mutex_unlock(ws->mutex);
    
    bool should_live = false;
    if (accept_callback != NULL)
        should_live = accept_callback(ws, new_web_connection, accept_callback_ctx);
    
    if (!should_live) {
        web_server_connection_destroy(new_web_connection);
        return false;
    }
    
    mutex_lock(ws->mutex);
    if (!ws->is_running) {
        mutex_unlock(ws->mutex);
        web_server_connection_destroy(new_web_connection);
        return false;
    }
    
    struct web_server_connection_t* connections = (struct web_server_connection_t*)realloc(ws->connections, sizeof(struct web_server_connection_t) * (ws->connection_count + 1));
    if (connections == NULL) {
        mutex_unlock(ws->mutex);
        web_server_connection_destroy(new_web_connection);
        return false;
    }
    
    ws->connections = connections;
    ws->connections[ws->connection_count].web_connection = new_web_connection;
    ws->connections[ws->connection_count].socket = new_socket;
    ws->connection_count++;
    
    socket_set_closed_callback(new_socket, _web_server_socket_closed, ws);
    web_server_connection_take_off(new_web_connection);
    mutex_unlock(ws->mutex);
    
    return true;
}

bool web_server_start(struct web_server_t* ws, uint16_t port) {
    
    if (ws == NULL)
        return false;
    
    mutex_lock(ws->mutex);
    if (ws->is_running) {
        mutex_unlock(ws->mutex);
        log_message(LOG_ERROR, "Cannot start: Server is already running");
        return false;
    }
    
    log_message(LOG_INFO, "Trying port %d", port);
    
    socket_p socket_ipv4 = NULL;
    socket_p socket_ipv6 = NULL;
    
    if ((ws->socket_types & sockaddr_type_inet_4) != 0)
        socket_ipv4 = _web_server_bind(ws, port, sockaddr_type_inet_4);
    if ((ws->socket_types & sockaddr_type_inet_6) != 0)
        socket_ipv6 = _web_server_bind(ws, port, sockaddr_type_inet_6);
    
    bool success = (((ws->socket_types & sockaddr_type_inet_4) == 0 || socket_ipv4 != NULL) &&
                    ((ws->socket_types & sockaddr_type_inet_6) == 0 || socket_ipv6 != NULL));
    
    if (success) {
        ws->socket_ipv4 = socket_ipv4;
        ws->socket_ipv6 = socket_ipv6;
        
        if (socket_ipv4 != NULL)
            socket_set_accept_callback(socket_ipv4, _web_server_socket_accept_callback, ws);
        if (socket_ipv6 != NULL)
            socket_set_accept_callback(socket_ipv6, _web_server_socket_accept_callback, ws);
        
        ws->is_running = true;
        mutex_unlock(ws->mutex);
        log_message(LOG_INFO, "Server started.");
        return true;
    }
    
    mutex_unlock(ws->mutex);
    
    if (socket_ipv4 != NULL)
        socket_destroy(socket_ipv4);
    if (socket_ipv6 != NULL)
        socket_destroy(socket_ipv6);
    
    log_message(LOG_ERROR, "Unable to start server on port %d", port);
    return false;
}

bool web_server_is_running(struct web_server_t* ws) {
    
    if (ws == NULL)
        return false;
    mutex_lock(ws->mutex);
    bool ret = ws->is_running;
    mutex_unlock(ws->mutex);
    return ret;
}

void web_server_stop(struct web_server_t* ws) {
    
    if (ws == NULL)
        return;
    
    mutex_lock(ws->mutex);
    if (!ws->is_running) {
        mutex_unlock(ws->mutex);
        return;
    }
    
    ws->is_running = false;
    socket_p socket_ipv4 = ws->socket_ipv4;
    socket_p socket_ipv6 = ws->socket_ipv6;
    ws->socket_ipv4 = NULL;
    ws->socket_ipv6 = NULL;
    mutex_unlock(ws->mutex);
    
    if (socket_ipv4 != NULL)
        socket_destroy(socket_ipv4);
    if (socket_ipv6 != NULL)
        socket_destroy(socket_ipv6);
    
    while (true) {
        mutex_lock(ws->mutex);
        if (ws->connection_count == 0) {
            free(ws->connections);
            ws->connections = NULL;
            mutex_unlock(ws->mutex);
            break;
        }
        socket_p connection_socket = ws->connections[0].socket;
        mutex_unlock(ws->mutex);
        
        if (connection_socket != NULL)
            socket_destroy(connection_socket);
        else {
            mutex_lock(ws->mutex);
            for (uint32_t i = 1 ; i < ws->connection_count ; i++)
                ws->connections[i - 1] = ws->connections[i];
            ws->connection_count--;
            mutex_unlock(ws->mutex);
        }
    }
    
    log_message(LOG_INFO, "Server stopped");
}

uint32_t web_server_get_connection_count(struct web_server_t* ws) {
    
    if (ws == NULL)
        return 0;
    mutex_lock(ws->mutex);
    uint32_t ret = ws->connection_count;
    mutex_unlock(ws->mutex);
    return ret;
}

struct sockaddr* web_server_get_local_end_point(struct web_server_t* ws, sockaddr_type socket_type) {
    
    if (ws == NULL)
        return NULL;
    
    mutex_lock(ws->mutex);
    socket_p socket = NULL;
    if (socket_type == sockaddr_type_inet_4 && (ws->socket_types & sockaddr_type_inet_4) != 0)
        socket = ws->socket_ipv4;
    else if (socket_type == sockaddr_type_inet_6 && (ws->socket_types & sockaddr_type_inet_6) != 0)
        socket = ws->socket_ipv6;
    
    struct sockaddr* end_point = socket != NULL ? socket_get_local_end_point(socket) : NULL;
    mutex_unlock(ws->mutex);
    return end_point;
}

void web_server_set_accept_callback(struct web_server_t* ws, web_server_accept_callback accept_callback, void* ctx) {
    
    if (ws == NULL)
        return;
    mutex_lock(ws->mutex);
    ws->accept_callback = accept_callback;
    ws->accept_callback_ctx = ctx;
    mutex_unlock(ws->mutex);
}
