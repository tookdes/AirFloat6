//
//  rtpsocket.c
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
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "rtpsocket.h"

struct rtp_socket_info_t {
    socket_p socket;
    bool is_data_socket;
};

struct rtp_socket_t {
    char* name;
    struct sockaddr* allowed_remote_end_point;
    struct rtp_socket_info_t** sockets;
    uint32_t sockets_count;
    rtp_socket_data_received_callback received_callback;
    void* received_callback_ctx;
    mutex_p mutex;
    bool destroying;
};

void _rtp_socket_socket_closed_callback(socket_p socket, void* ctx);
ssize_t _rtp_socket_socket_receive_callback(socket_p socket, const void* data, size_t data_size, struct sockaddr* remote_end_point, void* ctx);

struct rtp_socket_t* rtp_socket_create(const char* name, struct sockaddr* allowed_remote_end_point) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)malloc(sizeof(struct rtp_socket_t));
    if (rs == NULL)
        return NULL;
    bzero(rs, sizeof(struct rtp_socket_t));
    
    rs->mutex = mutex_create();
    if (rs->mutex == NULL) {
        free(rs);
        return NULL;
    }
    
    if (allowed_remote_end_point != NULL) {
        rs->allowed_remote_end_point = sockaddr_copy(allowed_remote_end_point);
        if (rs->allowed_remote_end_point == NULL) {
            mutex_destroy(rs->mutex);
            free(rs);
            return NULL;
        }
    }
    
    if (name != NULL) {
        rs->name = (char*)malloc(strlen(name) + 1);
        if (rs->name == NULL) {
            if (rs->allowed_remote_end_point != NULL)
                sockaddr_destroy(rs->allowed_remote_end_point);
            mutex_destroy(rs->mutex);
            free(rs);
            return NULL;
        }
        strcpy(rs->name, name);
    }
    
    return rs;
}

static struct rtp_socket_info_t* _rtp_socket_store_socket(struct rtp_socket_t* rs, socket_p socket, bool is_data_socket) {
    
    if (rs == NULL || socket == NULL)
        return NULL;
    
    struct rtp_socket_info_t* info = (struct rtp_socket_info_t*)malloc(sizeof(struct rtp_socket_info_t));
    if (info == NULL)
        return NULL;
    info->socket = socket;
    info->is_data_socket = is_data_socket;
    
    mutex_lock(rs->mutex);
    if (rs->destroying) {
        mutex_unlock(rs->mutex);
        free(info);
        return NULL;
    }
    
    struct rtp_socket_info_t** sockets = (struct rtp_socket_info_t**)realloc(rs->sockets, sizeof(struct rtp_socket_info_t*) * (rs->sockets_count + 1));
    if (sockets == NULL) {
        mutex_unlock(rs->mutex);
        free(info);
        return NULL;
    }
    
    rs->sockets = sockets;
    rs->sockets[rs->sockets_count++] = info;
    mutex_unlock(rs->mutex);
    
    return info;
}

static struct rtp_socket_info_t* _rtp_socket_detach_socket(struct rtp_socket_t* rs, socket_p socket) {
    
    if (rs == NULL || socket == NULL)
        return NULL;
    
    struct rtp_socket_info_t* info = NULL;
    
    mutex_lock(rs->mutex);
    for (uint32_t i = 0 ; i < rs->sockets_count ; i++) {
        if (rs->sockets[i]->socket == socket) {
            info = rs->sockets[i];
            for (uint32_t a = i + 1 ; a < rs->sockets_count ; a++)
                rs->sockets[a - 1] = rs->sockets[a];
            rs->sockets_count--;
            if (rs->sockets_count == 0) {
                free(rs->sockets);
                rs->sockets = NULL;
            }
            break;
        }
    }
    mutex_unlock(rs->mutex);
    
    return info;
}

static bool _rtp_socket_configure_socket(struct rtp_socket_t* rs, struct rtp_socket_info_t* info) {
    
    if (rs == NULL || info == NULL || info->socket == NULL)
        return false;
    
    /* Install the close callback before starting a receive worker so an
       immediate peer disconnect cannot leave an untracked socket behind. */
    socket_set_closed_callback(info->socket, _rtp_socket_socket_closed_callback, rs);
    if (info->is_data_socket)
        return socket_set_receive_callback(info->socket, _rtp_socket_socket_receive_callback, rs);
    
    return true;
}

static void _rtp_socket_rollback_setup_socket(struct rtp_socket_t* rs, socket_p socket) {
    
    if (socket == NULL)
        return;
    
    struct rtp_socket_info_t* info = _rtp_socket_detach_socket(rs, socket);
    if (info != NULL)
        free(info);
    
    /* The close callback may still run while socket_destroy joins a worker.
       With the info already detached it becomes a harmless no-op. */
    socket_destroy(socket);
}

void rtp_socket_destroy(struct rtp_socket_t* rs) {
    
    if (rs == NULL)
        return;
    
    mutex_lock(rs->mutex);
    if (rs->destroying) {
        mutex_unlock(rs->mutex);
        return;
    }
    rs->destroying = true;
    
    struct rtp_socket_info_t** sockets = rs->sockets;
    uint32_t sockets_count = rs->sockets_count;
    rs->sockets = NULL;
    rs->sockets_count = 0;
    rs->received_callback = NULL;
    rs->received_callback_ctx = NULL;
    mutex_unlock(rs->mutex);
    
    /* Detach close callbacks before destroying the sockets. Receive workers
       may still enter the RTP receive callback while socket_destroy joins
       them, but destroying=true makes that callback return without touching
       user state, and rs stays alive until all workers have exited. */
    for (uint32_t i = 0 ; i < sockets_count ; i++) {
        if (sockets[i] != NULL) {
            if (sockets[i]->socket != NULL) {
                socket_set_closed_callback(sockets[i]->socket, NULL, NULL);
                socket_destroy(sockets[i]->socket);
            }
            free(sockets[i]);
        }
    }
    free(sockets);
    
    if (rs->allowed_remote_end_point != NULL)
        sockaddr_destroy(rs->allowed_remote_end_point);
    free(rs->name);
    mutex_destroy(rs->mutex);
    free(rs);
}

void _rtp_socket_socket_closed_callback(socket_p socket, void* ctx) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)ctx;
    if (rs == NULL || socket == NULL)
        return;
    
    struct rtp_socket_info_t* info = _rtp_socket_detach_socket(rs, socket);
    if (info != NULL) {
        free(info);
        socket_destroy(socket);
    }
}

ssize_t _rtp_socket_socket_receive_callback(socket_p socket, const void* data, size_t data_size, struct sockaddr* remote_end_point, void* ctx) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)ctx;
    if (rs == NULL || data == NULL)
        return -1;
    
    rtp_socket_data_received_callback callback = NULL;
    void* callback_ctx = NULL;
    bool allowed = false;
    
    mutex_lock(rs->mutex);
    if (!rs->destroying) {
        allowed = (rs->allowed_remote_end_point == NULL ||
                   (remote_end_point != NULL && sockaddr_equals_host(remote_end_point, rs->allowed_remote_end_point)));
        if (allowed) {
            callback = rs->received_callback;
            callback_ctx = rs->received_callback_ctx;
        }
    }
    mutex_unlock(rs->mutex);
    
    if (allowed && callback != NULL)
        return callback(rs, socket, data, data_size, callback_ctx);
    
    return (ssize_t)data_size;
}

bool _rtp_socket_accept_callback(socket_p socket, socket_p new_socket, void* ctx) {
    
    struct rtp_socket_t* rs = (struct rtp_socket_t*)ctx;
    if (rs == NULL || new_socket == NULL)
        return false;
    
    struct sockaddr* remote_end_point = socket_get_remote_end_point(new_socket);
    
    mutex_lock(rs->mutex);
    bool allowed = !rs->destroying &&
        (rs->allowed_remote_end_point == NULL ||
         (remote_end_point != NULL && sockaddr_equals_host(remote_end_point, rs->allowed_remote_end_point)));
    mutex_unlock(rs->mutex);
    
    if (!allowed)
        return false;
    
    struct rtp_socket_info_t* info = _rtp_socket_store_socket(rs, new_socket, true);
    if (info == NULL)
        return false;
    
    if (!_rtp_socket_configure_socket(rs, info)) {
        struct rtp_socket_info_t* detached = _rtp_socket_detach_socket(rs, new_socket);
        if (detached != NULL)
            free(detached);
        log_message(LOG_ERROR, "Unable to start accepted RTP receive worker");
        return false;
    }
    
    return true;
}

bool rtp_socket_setup(struct rtp_socket_t* rs, struct sockaddr* local_end_point) {
    
    if (rs == NULL || local_end_point == NULL)
        return false;
    
    socket_p udp_socket = socket_create("RTP UDP Socket", true);
    socket_p tcp_socket = socket_create("RTP TCP Listen Socket", false);
    if (udp_socket == NULL || tcp_socket == NULL) {
        socket_destroy(udp_socket);
        socket_destroy(tcp_socket);
        return false;
    }
    
    if (!socket_bind(udp_socket, local_end_point) || !socket_bind(tcp_socket, local_end_point)) {
        socket_destroy(udp_socket);
        socket_destroy(tcp_socket);
        return false;
    }
    
    struct rtp_socket_info_t* udp_info = _rtp_socket_store_socket(rs, udp_socket, true);
    if (udp_info == NULL) {
        socket_destroy(udp_socket);
        socket_destroy(tcp_socket);
        return false;
    }
    
    struct rtp_socket_info_t* tcp_info = _rtp_socket_store_socket(rs, tcp_socket, false);
    if (tcp_info == NULL) {
        struct rtp_socket_info_t* detached = _rtp_socket_detach_socket(rs, udp_socket);
        free(detached);
        socket_destroy(udp_socket);
        socket_destroy(tcp_socket);
        return false;
    }
    
    bool success = _rtp_socket_configure_socket(rs, udp_info);
    if (success)
        success = _rtp_socket_configure_socket(rs, tcp_info);
    if (success)
        success = socket_set_accept_callback(tcp_socket, _rtp_socket_accept_callback, rs);
    
    if (!success) {
        log_message(LOG_ERROR, "Unable to start RTP socket workers");
        _rtp_socket_rollback_setup_socket(rs, udp_socket);
        _rtp_socket_rollback_setup_socket(rs, tcp_socket);
        return false;
    }
    
    return true;
}

void rtp_socket_set_data_received_callback(struct rtp_socket_t* rs, rtp_socket_data_received_callback callback, void* ctx) {
    
    if (rs == NULL)
        return;
    mutex_lock(rs->mutex);
    if (!rs->destroying) {
        rs->received_callback = callback;
        rs->received_callback_ctx = ctx;
    }
    mutex_unlock(rs->mutex);
}

void rtp_socket_send_to(struct rtp_socket_t* rs, struct sockaddr* dst, const void* buffer, uint32_t size) {
    
    if (rs == NULL || dst == NULL || buffer == NULL || size == 0)
        return;
    
    mutex_lock(rs->mutex);
    if (!rs->destroying) {
        for (uint32_t i = 0 ; i < rs->sockets_count ; i++) {
            if (rs->sockets[i] != NULL && rs->sockets[i]->is_data_socket)
                socket_send_to(rs->sockets[i]->socket, dst, buffer, size);
        }
    }
    mutex_unlock(rs->mutex);
}

uint16_t rtp_socket_get_local_port(rtp_socket_p rs) {
    
    if (rs == NULL)
        return 0;
    
    uint16_t port = 0;
    mutex_lock(rs->mutex);
    if (!rs->destroying) {
        for (uint32_t i = 0 ; i < rs->sockets_count ; i++) {
            if (rs->sockets[i] != NULL && !rs->sockets[i]->is_data_socket) {
                struct sockaddr* end_point = socket_get_local_end_point(rs->sockets[i]->socket);
                if (end_point != NULL)
                    port = sockaddr_get_port(end_point);
                break;
            }
        }
    }
    mutex_unlock(rs->mutex);
    
    return port;
}
