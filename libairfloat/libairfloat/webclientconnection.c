//
//  webclientconnection.c
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

#include "log.h"
#include "mutex.h"
#include "socket.h"

#include "webclientconnection.h"

struct web_client_connection_t {
    socket_p socket;
    mutex_p mutex;
    bool destroying;
    web_request_p* requests;
    uint32_t requests_count;
    struct {
        web_client_connection_connected_callback connected;
        web_client_connection_connect_failed_callback connect_failed;
        web_client_connection_response_received_callback response_received;
        web_client_connection_disconnected_callback disconnected;
        struct {
            void* connected;
            void* connect_failed;
            void* response_received;
            void* disconnected;
        } ctx;
    } callbacks;
};

bool _web_client_connection_send_next_request(struct web_client_connection_t* wc) {
    
    if (wc == NULL)
        return false;
    
    socket_p failed_socket = NULL;
    web_client_connection_disconnected_callback disconnected_callback = NULL;
    void* disconnected_callback_ctx = NULL;
    
    mutex_lock(wc->mutex);
    
    if (!wc->destroying && wc->socket != NULL && socket_is_connected(wc->socket) && wc->requests_count > 0) {
        
        size_t request_len = web_request_write(wc->requests[0], NULL, 0);
        if (request_len > 0) {
            char* data = (char*)malloc(request_len);
            if (data != NULL) {
                size_t written = web_request_write(wc->requests[0], data, request_len);
                if (written == request_len) {
                    log_data(LOG_INFO, data, request_len);
                    ssize_t sent = socket_send(wc->socket, data, request_len);
                    if (sent < 0 || (size_t)sent != request_len) {
                        log_message(LOG_ERROR, "Incomplete DACP request write (%d/%d bytes)", sent, request_len);
                        
                        /* Do not close while wc->mutex is held: DACP's
                           disconnected callback destroys this object. Detach
                           the socket and suppress its callback first, then
                           destroy it after releasing the client lock. */
                        failed_socket = wc->socket;
                        wc->socket = NULL;
                        socket_set_closed_callback(failed_socket, NULL, NULL);
                        disconnected_callback = wc->callbacks.disconnected;
                        disconnected_callback_ctx = wc->callbacks.ctx.disconnected;
                    }
                }
                free(data);
            }
        }

    }
    
    mutex_unlock(wc->mutex);
    
    if (failed_socket != NULL) {
        socket_destroy(failed_socket);
        if (disconnected_callback != NULL)
            disconnected_callback(wc, disconnected_callback_ctx);
        /* The disconnected callback is allowed to destroy wc. */
        return false;
    }
    
    return true;
}

void _web_client_connection_socket_connected_callback(socket_p socket, void* ctx) {
    
    struct web_client_connection_t* wc = (struct web_client_connection_t*)ctx;
    
    if (wc != NULL && wc->callbacks.connected != NULL)
        wc->callbacks.connected(wc, wc->callbacks.ctx.connected);
    
}

void _web_client_connection_socket_connect_failed_callback(socket_p socket, void* ctx) {
    
    struct web_client_connection_t* wc = (struct web_client_connection_t*)ctx;
    
    if (wc != NULL && wc->callbacks.connect_failed != NULL)
        wc->callbacks.connect_failed(wc, wc->callbacks.ctx.connect_failed);
    
}

ssize_t _web_client_connection_socket_receive_callback(socket_p socket, const void* data, size_t data_size, struct sockaddr* remote_end_point, void* ctx) {
    
    struct web_client_connection_t* wc = (struct web_client_connection_t*)ctx;
    if (wc == NULL)
        return -1;
    
    web_response_p response = web_response_create();
    if (response == NULL)
        return -1;
    
    ssize_t ret = web_response_parse(response, data, data_size);
    if (ret > 0) {
        
        web_request_p completed_request = NULL;
        web_client_connection_response_received_callback response_callback = NULL;
        void* response_callback_ctx = NULL;
        
        mutex_lock(wc->mutex);
        
        if (!wc->destroying && wc->requests_count > 0) {
            completed_request = wc->requests[0];
            for (uint32_t i = 1 ; i < wc->requests_count ; i++)
                wc->requests[i - 1] = wc->requests[i];
            wc->requests_count--;
            
            if (wc->requests_count == 0) {
                free(wc->requests);
                wc->requests = NULL;
            }
            
            response_callback = wc->callbacks.response_received;
            response_callback_ctx = wc->callbacks.ctx.response_received;
        }
        
        mutex_unlock(wc->mutex);
        
        if (completed_request == NULL) {
            web_response_destroy(response);
            return -1;
        }
        
        /* Send the next queued request while the connection is still known
           to be alive. A send failure can invoke the disconnected callback,
           which may destroy wc; in that case do not pass the stale connection
           to the response callback. */
        bool connection_alive = _web_client_connection_send_next_request(wc);
        
        if (connection_alive && response_callback != NULL)
            response_callback(wc, completed_request, response, response_callback_ctx);
        
        web_request_destroy(completed_request);
        web_response_destroy(response);
        return ret;
        
    }
    
    web_response_destroy(response);
    
    return ret;
    
}

void _web_connection_socket_closed_callback(socket_p socket, void* ctx) {
    
    struct web_client_connection_t* wc = (struct web_client_connection_t*)ctx;
    
    if (wc != NULL && wc->callbacks.disconnected != NULL)
        wc->callbacks.disconnected(wc, wc->callbacks.ctx.disconnected);
    
}

struct web_client_connection_t* web_client_connection_create() {
    
    struct web_client_connection_t* wc = (struct web_client_connection_t*)malloc(sizeof(struct web_client_connection_t));
    if (wc == NULL)
        return NULL;
    
    bzero(wc, sizeof(struct web_client_connection_t));
    
    wc->mutex = mutex_create();
    if (wc->mutex == NULL) {
        free(wc);
        return NULL;
    }
    
    return wc;
    
}

void web_client_connection_destroy(struct web_client_connection_t* wc) {
    
    if (wc == NULL)
        return;
    
    mutex_lock(wc->mutex);
    
    if (wc->destroying) {
        mutex_unlock(wc->mutex);
        return;
    }
    
    wc->destroying = true;
    socket_p socket = wc->socket;
    wc->socket = NULL;
    
    web_request_p* requests = wc->requests;
    uint32_t requests_count = wc->requests_count;
    wc->requests = NULL;
    wc->requests_count = 0;
    
    mutex_unlock(wc->mutex);
    
    if (socket != NULL) {
        /* An explicit destroy owns the disconnect; do not recurse through
           the client disconnected callback while tearing the object down. */
        socket_set_closed_callback(socket, NULL, NULL);
        socket_destroy(socket);
    }
    
    for (uint32_t i = 0 ; i < requests_count ; i++)
        web_request_destroy(requests[i]);
    free(requests);
    
    mutex_destroy(wc->mutex);
    free(wc);
    
}

void web_client_connection_set_connected_callback(struct web_client_connection_t* wc, web_client_connection_connected_callback callback, void* ctx) {
    
    if (wc == NULL)
        return;
    wc->callbacks.connected = callback;
    wc->callbacks.ctx.connected = ctx;
    
}

void web_client_connection_set_connect_failed_callback(struct web_client_connection_t* wc, web_client_connection_connect_failed_callback callback, void* ctx) {
    
    if (wc == NULL)
        return;
    wc->callbacks.connect_failed = callback;
    wc->callbacks.ctx.connect_failed = ctx;
    
}

void web_client_connection_set_response_received_callback(struct web_client_connection_t* wc, web_client_connection_response_received_callback callback, void* ctx) {
    
    if (wc == NULL)
        return;
    wc->callbacks.response_received = callback;
    wc->callbacks.ctx.response_received = ctx;
    
}

void web_client_connection_set_disconneced_callback(struct web_client_connection_t* wc, web_client_connection_disconnected_callback callback, void* ctx) {
    
    if (wc == NULL)
        return;
    wc->callbacks.disconnected = callback;
    wc->callbacks.ctx.disconnected = ctx;
    
}

void web_client_connection_connect(struct web_client_connection_t* wc, struct sockaddr* end_point) {
    
    if (wc == NULL || end_point == NULL)
        return;
    
    mutex_lock(wc->mutex);
    bool should_connect = (!wc->destroying && wc->socket == NULL);
    mutex_unlock(wc->mutex);
    
    if (should_connect) {
        
        socket_p socket = socket_create("Web connection", false);
        if (socket == NULL)
            return;
        
        socket_set_connected_callback(socket, _web_client_connection_socket_connected_callback, wc);
        socket_set_connect_failed_callback(socket, _web_client_connection_socket_connect_failed_callback, wc);
        socket_set_receive_callback(socket, _web_client_connection_socket_receive_callback, wc);
        socket_set_closed_callback(socket, _web_connection_socket_closed_callback, wc);
        
        mutex_lock(wc->mutex);
        if (!wc->destroying && wc->socket == NULL) {
            wc->socket = socket;
            socket = NULL;
        }
        socket_p active_socket = wc->socket;
        mutex_unlock(wc->mutex);
        
        if (socket != NULL)
            socket_destroy(socket);
        else if (active_socket != NULL)
            socket_connect(active_socket, end_point);
        
    }
    
}

bool web_client_connection_is_connected(struct web_client_connection_t* wc) {
    
    if (wc == NULL)
        return false;
    
    mutex_lock(wc->mutex);
    bool ret = (!wc->destroying && wc->socket != NULL && socket_is_connected(wc->socket));
    mutex_unlock(wc->mutex);
    
    return ret;
    
}

void web_client_connection_send_request(web_client_connection_p wc, web_request_p request) {
    
    if (wc == NULL || request == NULL)
        return;
    
    web_request_p request_copy = web_request_copy(request);
    if (request_copy == NULL)
        return;
    
    bool send = false;
    
    mutex_lock(wc->mutex);
    
    if (!wc->destroying && wc->socket != NULL && socket_is_connected(wc->socket)) {
        web_request_p* requests = (web_request_p*)realloc(wc->requests, sizeof(web_request_p) * (wc->requests_count + 1));
        if (requests != NULL) {
            wc->requests = requests;
            wc->requests[wc->requests_count] = request_copy;
            wc->requests_count++;
            request_copy = NULL;
            send = (wc->requests_count == 1);
        }
    }
    
    mutex_unlock(wc->mutex);
    
    if (request_copy != NULL)
        web_request_destroy(request_copy);
    
    if (send)
        _web_client_connection_send_next_request(wc);
    
}
