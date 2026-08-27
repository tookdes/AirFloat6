//
//  webserverconnection.c
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "log.h"
#include "mutex.h"
#include "socket.h"
#include "sockaddr.h"
#include "webtools.h"
#include "webheaders.h"
#include "webrequest.h"
#include "webserverconnection.h"

#define MAX(x,y) (x > y ? x : y)

struct web_server_connection_t {
    mutex_p mutex;
    bool is_connected;
    bool has_taken_off;
    bool close_in_progress;
    bool destroy_pending;
    bool destroying;
    socket_p socket;
    web_server_p server;
    web_server_connection_request_callback request_callback;
    void* request_callback_ctx;
    web_server_connection_closed_callback closed_callback;
    void* closed_callback_ctx;
};

ssize_t _web_server_connection_socket_recieve_callback(socket_p socket, const void* data, size_t data_size, struct sockaddr* remote_end_point, void* ctx) {
    
    struct web_server_connection_t* wc = (struct web_server_connection_t*)ctx;
    if (wc == NULL)
        return -1;
    
    web_request_p request = web_request_create();
    if (request == NULL)
        return -1;
    
    ssize_t ret = web_request_parse(request, data, data_size);
    if (ret > 0) {
        web_server_connection_request_callback request_callback = NULL;
        void* request_callback_ctx = NULL;
        
        mutex_lock(wc->mutex);
        if (!wc->destroying) {
            request_callback = wc->request_callback;
            request_callback_ctx = wc->request_callback_ctx;
        }
        mutex_unlock(wc->mutex);
        
        if (request_callback != NULL)
            request_callback(wc, request, request_callback_ctx);
    }
    
    web_request_destroy(request);
    return ret;
}

struct web_server_connection_t* web_server_connection_create(socket_p socket, web_server_p server) {
    
    if (socket == NULL || server == NULL)
        return NULL;
    
    struct web_server_connection_t* wc = (struct web_server_connection_t*)malloc(sizeof(struct web_server_connection_t));
    if (wc == NULL)
        return NULL;
    bzero(wc, sizeof(struct web_server_connection_t));
    
    wc->socket = socket;
    wc->server = server;
    wc->mutex = mutex_create();
    if (wc->mutex == NULL) {
        free(wc);
        return NULL;
    }
    
    return wc;
}

void web_server_connection_destroy(struct web_server_connection_t* wc) {
    
    if (wc == NULL)
        return;
    
    web_server_connection_closed_callback closed_callback = NULL;
    void* closed_callback_ctx = NULL;
    
    mutex_lock(wc->mutex);
    
    if (wc->destroying) {
        mutex_unlock(wc->mutex);
        return;
    }
    
    if (wc->close_in_progress) {
        wc->destroy_pending = true;
        mutex_unlock(wc->mutex);
        return;
    }
    
    wc->destroying = true;
    
    if (wc->is_connected) {
        wc->is_connected = false;
        closed_callback = wc->closed_callback;
        closed_callback_ctx = wc->closed_callback_ctx;
    }
    
    mutex_unlock(wc->mutex);
    
    if (closed_callback != NULL)
        closed_callback(wc, closed_callback_ctx);
    
    if (wc->socket != NULL)
        socket_close(wc->socket);
    
    mutex_destroy(wc->mutex);
    free(wc);
}

void web_server_connection_set_request_callback(struct web_server_connection_t* wc, web_server_connection_request_callback request_callback, void* ctx) {
    
    if (wc == NULL)
        return;
    mutex_lock(wc->mutex);
    wc->request_callback = request_callback;
    wc->request_callback_ctx = ctx;
    mutex_unlock(wc->mutex);
}

void web_server_connection_set_closed_callback(struct web_server_connection_t* wc, web_server_connection_closed_callback closed_callback, void* ctx) {
    
    if (wc == NULL)
        return;
    mutex_lock(wc->mutex);
    wc->closed_callback = closed_callback;
    wc->closed_callback_ctx = ctx;
    mutex_unlock(wc->mutex);
}

void web_server_connection_send_response(web_server_connection_p wc, web_response_p response, const char* protocol, bool close_after_send) {
    
    if (wc == NULL || response == NULL || protocol == NULL)
        return;
    
    size_t content_length = web_response_get_content(response, NULL, 0);
    if (content_length > 0)
        web_headers_set_value(web_response_get_headers(response), "Content-Length", "%lu", (unsigned long)content_length);
    
    size_t response_length = web_response_write(response, protocol, NULL, 0);
    if (response_length == 0 || content_length > SIZE_MAX - response_length)
        return;
    
    size_t total_length = response_length + content_length;
    char* buffer = (char*)malloc(total_length > 0 ? total_length : 1);
    if (buffer == NULL)
        return;
    
    if (web_response_write(response, protocol, buffer, response_length) != response_length) {
        free(buffer);
        return;
    }
    
    if (content_length > 0)
        web_response_get_content(response, buffer + response_length, content_length);
    
    socket_send(wc->socket, buffer, total_length);
    log_data(LOG_INFO, buffer, total_length);
    free(buffer);
    
    if (close_after_send)
        socket_close(wc->socket);
}

bool web_server_connection_is_connected(struct web_server_connection_t* wc) {
    
    if (wc == NULL)
        return false;
    mutex_lock(wc->mutex);
    bool ret = wc->is_connected && !wc->destroying;
    mutex_unlock(wc->mutex);
    return ret;
}

void web_server_connection_take_off(struct web_server_connection_t* wc) {
    
    if (wc == NULL || wc->socket == NULL)
        return;
    
    mutex_lock(wc->mutex);
    if (!wc->destroying) {
        wc->has_taken_off = true;
        wc->is_connected = true;
        socket_set_receive_callback(wc->socket, _web_server_connection_socket_recieve_callback, wc);
    }
    mutex_unlock(wc->mutex);
    
    struct sockaddr* remote_end_point = socket_get_remote_end_point(wc->socket);
    const char* ip = remote_end_point != NULL ? sockaddr_get_host(remote_end_point) : NULL;
    log_message(LOG_INFO, "RAOPConnection (%p) took over connection from %s:%d", wc, (ip != NULL ? ip : "unknown"), (remote_end_point != NULL ? sockaddr_get_port(remote_end_point) : 0));
}

void web_server_connection_close(struct web_server_connection_t* wc) {
    
    if (wc == NULL)
        return;
    
    bool should_close = false;
    
    mutex_lock(wc->mutex);
    if (wc->is_connected && !wc->destroying) {
        log_message(LOG_INFO, "Client disconnected");
        wc->is_connected = false;
        wc->close_in_progress = true;
        should_close = true;
    }
    mutex_unlock(wc->mutex);
    
    if (!should_close)
        return;
    
    socket_close(wc->socket);
    
    mutex_lock(wc->mutex);
    wc->close_in_progress = false;
    bool should_destroy = wc->destroy_pending && !wc->destroying;
    wc->destroy_pending = false;
    mutex_unlock(wc->mutex);
    
    if (should_destroy)
        web_server_connection_destroy(wc);
}

struct sockaddr* web_server_connection_get_local_end_point(struct web_server_connection_t* wc) {
    return (wc != NULL && wc->socket != NULL) ? socket_get_local_end_point(wc->socket) : NULL;
}

struct sockaddr* web_server_connection_get_remote_end_point(struct web_server_connection_t* wc) {
    return (wc != NULL && wc->socket != NULL) ? socket_get_remote_end_point(wc->socket) : NULL;
}

const char* web_server_connection_get_host(struct web_server_connection_t* wc) {
    struct sockaddr* end_point = web_server_connection_get_remote_end_point(wc);
    return end_point != NULL ? sockaddr_get_host(end_point) : NULL;
}

uint16_t web_server_connection_get_port(struct web_server_connection_t* wc) {
    struct sockaddr* end_point = web_server_connection_get_remote_end_point(wc);
    return end_point != NULL ? sockaddr_get_port(end_point) : 0;
}
