//
//  Socket.cpp
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
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <string.h>

#include "log.h"
#include "mutex.h"
#include "thread.h"

#include "sockaddr.h"
#include "socket.h"

struct socket_t {
    char* name;
    bool is_udp;
    int socket;
    bool is_connected;
    bool close_in_progress;
    bool destroy_pending;
    bool destroying;
    bool accept_loop_active;
    bool receive_loop_active;
    mutex_p mutex;
    thread_p accept_thread;
    thread_p receive_thread;
    struct {
        socket_accept_callback accept;
        socket_connected_callback connected;
        socket_connect_failed_callback connect_failed;
        socket_receive_callback receive;
        socket_closed_callback closed;
        struct {
            void* accept;
            void* connected;
            void* connect_failed;
            void* receive;
            void* closed;
        } ctx;
    } callbacks;
    struct sockaddr* local_end_point;
    struct sockaddr* remote_end_point;
};

void _socket_enable_tcp_keepalive(int socket_fd) {
    
    if (socket_fd < 0)
        return;
    
    int enabled = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    
#if defined(SO_NOSIGPIPE)
    setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    
    /* Keep paused RAOP sessions alive while allowing dead Wi-Fi peers to be
       detected promptly. Darwin exposes the idle time as TCP_KEEPALIVE. */
#if defined(TCP_KEEPALIVE)
    int keepalive_idle = 30;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPALIVE, &keepalive_idle, sizeof(keepalive_idle));
#elif defined(TCP_KEEPIDLE)
    int keepalive_idle = 30;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_idle, sizeof(keepalive_idle));
#endif
    
#if defined(TCP_KEEPINTVL)
    int keepalive_interval = 10;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_interval, sizeof(keepalive_interval));
#endif
    
#if defined(TCP_KEEPCNT)
    int keepalive_count = 3;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_count, sizeof(keepalive_count));
#endif
    
}

void _socket_set_loop_name(struct socket_t* s, const char* name) {
    
    if (s != NULL && s->name != NULL && name != NULL) {
        
        size_t len = strlen(s->name) + strlen(name) + 4;
        char t_name[len];
        sprintf(t_name, "%s - %s", s->name, name);
        thread_set_name(t_name);
        
    }
    
}

void _socket_worker_finished(struct socket_t* s, bool accept_worker) {
    
    if (s == NULL)
        return;
    
    bool should_destroy = false;
    
    mutex_lock(s->mutex);
    
    if (accept_worker)
        s->accept_loop_active = false;
    else
        s->receive_loop_active = false;
    
    should_destroy = s->destroy_pending && !s->close_in_progress && !s->destroying &&
                     !s->accept_loop_active && !s->receive_loop_active;
    if (should_destroy)
        s->destroy_pending = false;
    
    mutex_unlock(s->mutex);
    
    if (should_destroy)
        socket_destroy(s);
    
}

void _socket_accept_loop(void* ctx) {
    
    struct socket_t* s = (struct socket_t*)ctx;
    if (s == NULL)
        return;
   
    mutex_lock(s->mutex);
    _socket_set_loop_name(s, "Accept Loop");
    
    s->accept_loop_active = true;
    if (s->socket < 0 || s->destroy_pending || s->destroying || s->close_in_progress) {
        mutex_unlock(s->mutex);
        socket_close(s);
        _socket_worker_finished(s, true);
        return;
    }
    s->is_connected = true;
    
    int new_socket_fd = 0;
    do {
        
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        mutex_unlock(s->mutex);
        new_socket_fd = accept(s->socket, (struct sockaddr*)&client_addr, &addr_len);
        mutex_lock(s->mutex);
        
        if (new_socket_fd >= 0) {
            
            _socket_enable_tcp_keepalive(new_socket_fd);
            
            struct socket_t* new_socket = (struct socket_t*)malloc(sizeof(struct socket_t));
            if (new_socket == NULL) {
                close(new_socket_fd);
                continue;
            }
            bzero(new_socket, sizeof(struct socket_t));
            
            new_socket->socket = new_socket_fd;
            new_socket->mutex = mutex_create();
            if (new_socket->mutex == NULL) {
                close(new_socket_fd);
                free(new_socket);
                continue;
            }
            new_socket->is_connected = true;
            
            bool accept = false;
            if (s->callbacks.accept != NULL) {
                mutex_unlock(s->mutex);
                accept = s->callbacks.accept(s, new_socket, s->callbacks.ctx.accept);
                mutex_lock(s->mutex);
            }
            
            if (!accept)
                socket_destroy(new_socket);
            
        }
        
    } while (new_socket_fd >= 0);
    mutex_unlock(s->mutex);
    
    socket_close(s);
    _socket_worker_finished(s, true);
    
}

void _socket_receive_loop(void* ctx) {
    
    struct socket_t* s = (struct socket_t*)ctx;
    if (s == NULL)
        return;
    
    mutex_lock(s->mutex);
    _socket_set_loop_name(s, "Receive Loop");
    
    s->receive_loop_active = true;
    if (s->socket < 0 || s->destroy_pending || s->destroying || s->close_in_progress) {
        mutex_unlock(s->mutex);
        socket_close(s);
        _socket_worker_finished(s, false);
        return;
    }
    s->is_connected = true;
    
    void* buffer = NULL;
    size_t buffer_size = 0;
    size_t write_pos = 0;
    ssize_t processed = 0;
    
    ssize_t read = 0;
    
    do {
        
        if (buffer_size - write_pos < 16384) {
            size_t new_buffer_size = buffer_size + 16384;
            if (new_buffer_size < buffer_size) {
                log_message(LOG_ERROR, "Socket receive buffer size overflow");
                processed = -1;
                break;
            }
            
            void* new_buffer = realloc(buffer, new_buffer_size);
            if (new_buffer == NULL) {
                log_message(LOG_ERROR, "Unable to grow socket receive buffer");
                processed = -1;
                break;
            }
            
            buffer = new_buffer;
            buffer_size = new_buffer_size;
        }
        
        if (s->is_udp) {
            
            struct sockaddr_storage remote_addr;
            socklen_t remote_addr_len = sizeof(struct sockaddr_storage);
            mutex_unlock(s->mutex);
            read = recvfrom(s->socket, (char*)buffer + write_pos, buffer_size - write_pos, 0, (struct sockaddr*) &remote_addr, &remote_addr_len);
            mutex_lock(s->mutex);
            
            if (read > 0) {
                if (s->remote_end_point != NULL) {
                    sockaddr_destroy(s->remote_end_point);
                    s->remote_end_point = NULL;
                }
                
                s->remote_end_point = sockaddr_copy((struct sockaddr*) &remote_addr);
            }
            
        } else {
            mutex_unlock(s->mutex);
            read = recv(s->socket, (char*)buffer + write_pos, buffer_size - write_pos, 0);
            mutex_lock(s->mutex);
        }
        
        if (read > 0) {
            
            write_pos += (size_t)read;
            processed = (ssize_t)write_pos;
            
            if (s->callbacks.receive != NULL) {
                mutex_unlock(s->mutex);
                processed = s->callbacks.receive(s, buffer, write_pos, s->remote_end_point, s->callbacks.ctx.receive);
                mutex_lock(s->mutex);
            }
            
            if (processed > 0) {
                if ((size_t)processed > write_pos) {
                    log_message(LOG_ERROR, "Socket receive callback consumed beyond buffer");
                    processed = -1;
                } else {
                    memmove(buffer, (char*)buffer + processed, write_pos - (size_t)processed);
                    write_pos -= (size_t)processed;
                }
            }
            
        }
        
    } while (read > 0 && processed >= 0);
    
    if (buffer != NULL)
        free(buffer);
    
    mutex_unlock(s->mutex);
    
    socket_close(s);
    _socket_worker_finished(s, false);
    
}

void _socket_connect(void* ctx) {
    
    struct socket_t* s = (struct socket_t*)ctx;
    if (s == NULL)
        return;
    
    mutex_lock(s->mutex);
    _socket_set_loop_name(s, "Connect Loop");
    s->receive_loop_active = true;
    
    int socket_fd = s->socket;
    struct sockaddr* remote_end_point = s->remote_end_point != NULL ? sockaddr_copy(s->remote_end_point) : NULL;
    bool can_connect = socket_fd >= 0 && remote_end_point != NULL &&
                       !s->destroy_pending && !s->destroying && !s->close_in_progress;
    mutex_unlock(s->mutex);
    
    if (!can_connect) {
        if (remote_end_point != NULL)
            sockaddr_destroy(remote_end_point);
        socket_close(s);
        _socket_worker_finished(s, false);
        return;
    }
    
    int connect_result = connect(socket_fd, remote_end_point, remote_end_point->sa_len);
    int connect_error = errno;
    sockaddr_destroy(remote_end_point);
    
    if (connect_result == 0) {
        socket_connected_callback connected_callback = NULL;
        void* connected_callback_ctx = NULL;
        
        mutex_lock(s->mutex);
        if (s->socket == socket_fd && !s->destroy_pending && !s->destroying && !s->close_in_progress) {
            /* Report the socket as connected before notifying the client so a
               request sent from its connected callback is not discarded. */
            s->is_connected = true;
            connected_callback = s->callbacks.connected;
            connected_callback_ctx = s->callbacks.ctx.connected;
        }
        mutex_unlock(s->mutex);
        
        if (connected_callback != NULL)
            connected_callback(s, connected_callback_ctx);
        
        mutex_lock(s->mutex);
        bool should_receive = s->socket == socket_fd && s->is_connected &&
                              !s->destroy_pending && !s->destroying && !s->close_in_progress;
        mutex_unlock(s->mutex);
        
        if (should_receive) {
            _socket_receive_loop(ctx);
            return;
        }
    } else {
        socket_connect_failed_callback connect_failed_callback = NULL;
        void* connect_failed_callback_ctx = NULL;
        
        mutex_lock(s->mutex);
        bool should_notify = s->socket == socket_fd && !s->destroy_pending && !s->destroying && !s->close_in_progress;
        if (should_notify) {
            connect_failed_callback = s->callbacks.connect_failed;
            connect_failed_callback_ctx = s->callbacks.ctx.connect_failed;
        }
        mutex_unlock(s->mutex);
        
        if (should_notify)
            log_message(LOG_ERROR, "Unable to connect (%s)", strerror(connect_error));
        if (connect_failed_callback != NULL)
            connect_failed_callback(s, connect_failed_callback_ctx);
    }
    
    /* A failed connection or a callback-triggered close ends this worker.
       Closing also clears the thread handle so a later connect gets a fresh fd. */
    socket_close(s);
    _socket_worker_finished(s, false);
}

struct socket_t* socket_create(const char* name, bool is_udp) {
    
    struct socket_t* s = (struct socket_t*)malloc(sizeof(struct socket_t));
    if (s == NULL)
        return NULL;
    bzero(s, sizeof(struct socket_t));
    
    s->is_udp = is_udp;
    s->socket = -1;
    s->mutex = mutex_create();
    if (s->mutex == NULL) {
        free(s);
        return NULL;
    }
    
    if (name != NULL) {
        s->name = (char*)malloc(strlen(name) + 1);
        if (s->name == NULL) {
            mutex_destroy(s->mutex);
            free(s);
            return NULL;
        }
        strcpy(s->name, name);
    }
    
    return s;
    
}

void socket_destroy(struct socket_t* s) {
    
    if (s == NULL)
        return;
    
    mutex_lock(s->mutex);
    
    if (s->destroying) {
        mutex_unlock(s->mutex);
        return;
    }
    
    if (s->close_in_progress) {
        s->destroy_pending = true;
        mutex_unlock(s->mutex);
        return;
    }
    
    if (s->accept_loop_active || s->receive_loop_active) {
        s->destroy_pending = true;
        mutex_unlock(s->mutex);
        socket_close(s);
        return;
    }
    
    s->destroying = true;
    
    mutex_unlock(s->mutex);
    
    socket_close(s);
    
    mutex_lock(s->mutex);
    
    if (s->local_end_point != NULL) {
        sockaddr_destroy(s->local_end_point);
        s->local_end_point = NULL;
    }
    
    if (s->remote_end_point != NULL) {
        sockaddr_destroy(s->remote_end_point);
        s->remote_end_point = NULL;
    }
    
    if (s->name != NULL) {
        free(s->name);
        s->name = NULL;
    }
    
    mutex_unlock(s->mutex);
    
    mutex_destroy(s->mutex);
    
    free(s);
    
}

bool socket_bind(struct socket_t* s, struct sockaddr* end_point) {
    
    if (s == NULL || end_point == NULL)
        return false;
    
    struct sockaddr* ep = sockaddr_copy(end_point);
    if (ep == NULL)
        return false;
    
    if (s->local_end_point != NULL)
        sockaddr_destroy(s->local_end_point);
    s->local_end_point = ep;
    
    if (s->socket < 0) {
        
        s->socket = socket(ep->sa_family, (s->is_udp ? SOCK_DGRAM : SOCK_STREAM), (s->is_udp ? IPPROTO_UDP : IPPROTO_TCP));
        
        if (s->socket < 0) {
            log_message(LOG_ERROR, "Socket creation error: %s", strerror(errno));
            return false;
        }
        
        if (!s->is_udp)
            _socket_enable_tcp_keepalive(s->socket);
        
        if (sockaddr_is_ipv6(ep)) {
            int32_t on = 1;
            setsockopt(s->socket, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        }
        
    }
    
    if (!s->is_udp) {
        int so_reuseaddr = 1;
        setsockopt(s->socket, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(int));
    }
    
    if (bind(s->socket, ep, ep->sa_len) == 0)
        return true;
    
    return false;
    
}

void socket_connect(struct socket_t* s, struct sockaddr* end_point) {
    
    if (s == NULL || end_point == NULL || s->is_connected || s->is_udp)
        return;
    
    if (s->socket < 0) {
        s->socket = socket(end_point->sa_family, SOCK_STREAM, IPPROTO_TCP);
        if (s->socket < 0) {
            log_message(LOG_ERROR, "Socket creation error: %s", strerror(errno));
            return;
        }
        _socket_enable_tcp_keepalive(s->socket);
    }
    
    struct sockaddr* remote_end_point = sockaddr_copy(end_point);
    if (remote_end_point == NULL)
        return;
    
    if (s->remote_end_point != NULL)
        sockaddr_destroy(s->remote_end_point);
    s->remote_end_point = remote_end_point;
    
    s->receive_thread = thread_create_a(_socket_connect, s);
    if (s->receive_thread == NULL) {
        log_message(LOG_ERROR, "Unable to create socket connect worker");
        close(s->socket);
        s->socket = -1;
        sockaddr_destroy(s->remote_end_point);
        s->remote_end_point = NULL;
    }
    
}

bool socket_set_accept_callback(struct socket_t* s, socket_accept_callback callback, void* ctx) {
    
    if (s == NULL)
        return false;
    
    s->callbacks.accept = callback;
    s->callbacks.ctx.accept = ctx;
    
    if (s->is_udp)
        return false;
    if (callback == NULL || s->accept_thread != NULL)
        return true;
    if (s->socket < 0)
        return false;
    
    if (listen(s->socket, 5) != 0) {
        log_message(LOG_ERROR, "Unable to listen on socket (%s)", strerror(errno));
        return false;
    }
    
    s->accept_thread = thread_create_a(_socket_accept_loop, s);
    if (s->accept_thread == NULL) {
        log_message(LOG_ERROR, "Unable to create socket accept worker");
        return false;
    }
    
    return true;
}

void socket_set_connected_callback(struct socket_t* s, socket_connected_callback callback, void* ctx) {
    
    if (s == NULL)
        return;
    s->callbacks.connected = callback;
    s->callbacks.ctx.connected = ctx;
    
}

void socket_set_connect_failed_callback(struct socket_t* s, socket_connect_failed_callback callback, void* ctx) {
    
    if (s == NULL)
        return;
    s->callbacks.connect_failed = callback;
    s->callbacks.ctx.connect_failed = ctx;
    
}

bool socket_set_receive_callback(struct socket_t* s, socket_receive_callback callback, void* ctx) {
    
    if (s == NULL)
        return false;
    
    s->callbacks.receive = callback;
    s->callbacks.ctx.receive = ctx;
    
    if (callback == NULL || s->receive_thread != NULL)
        return true;
    
    if (s->is_udp || s->is_connected) {
        s->receive_thread = thread_create_a(_socket_receive_loop, s);
        if (s->receive_thread == NULL) {
            log_message(LOG_ERROR, "Unable to create socket receive worker");
            return false;
        }
    }
    
    return true;
}

void socket_set_closed_callback(struct socket_t* s, socket_closed_callback callback, void* ctx) {
    
    if (s == NULL)
        return;
    s->callbacks.closed = callback;
    s->callbacks.ctx.closed = ctx;
    
}

ssize_t socket_send(struct socket_t* s, const void* buffer, size_t size) {
    
    if (s == NULL || buffer == NULL || size == 0)
        return 0;
    
    ssize_t ret = 0;
    
    mutex_lock(s->mutex);
    
    if (s->is_connected && s->socket >= 0) {
        const char* bytes = (const char*)buffer;
        size_t sent = 0;
        
        while (sent < size) {
            ssize_t written = send(s->socket, bytes + sent, size - sent, 0);
            if (written > 0) {
                sent += (size_t)written;
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            
            ret = (sent > 0 ? (ssize_t)sent : (written < 0 ? -1 : 0));
            break;
        }
        
        if (sent == size)
            ret = (ssize_t)sent;
    }
    
    mutex_unlock(s->mutex);
    
    return ret;
    
}

ssize_t socket_send_to(struct socket_t* s, struct sockaddr* end_point, const void* buffer, size_t size) {
    
    if (s == NULL || end_point == NULL || buffer == NULL || size == 0)
        return -1;
    
    mutex_lock(s->mutex);
    
    if (!s->is_udp) {
        mutex_unlock(s->mutex);
        return socket_send(s, buffer, size);
    }
    
    if (s->local_end_point == NULL || end_point->sa_family != s->local_end_point->sa_family || s->socket < 0) {
        mutex_unlock(s->mutex);
        return -1;
    }
    
    socklen_t len = end_point->sa_len;
    ssize_t ret = sendto(s->socket, buffer, size, 0, (struct sockaddr*) end_point, len);
    
    mutex_unlock(s->mutex);
    
    return ret;
    
}

void socket_close(struct socket_t* s) {
    
    if (s == NULL)
        return;
    
    int socket_fd = -1;
    thread_p accept_thread = NULL;
    thread_p receive_thread = NULL;
    socket_closed_callback closed_callback = NULL;
    void* closed_callback_ctx = NULL;
    bool should_notify = false;
    bool should_close = false;
    
    mutex_lock(s->mutex);
    
    if (s->close_in_progress) {
        mutex_unlock(s->mutex);
        return;
    }
    
    if (s->socket >= 0 || s->is_connected || s->accept_thread != NULL || s->receive_thread != NULL) {
        
        s->close_in_progress = true;
        should_close = true;
        should_notify = s->is_connected;
        s->is_connected = false;
        
        socket_fd = s->socket;
        s->socket = -1;
        
        accept_thread = s->accept_thread;
        s->accept_thread = NULL;
        
        receive_thread = s->receive_thread;
        s->receive_thread = NULL;
        
        if (should_notify) {
            closed_callback = s->callbacks.closed;
            closed_callback_ctx = s->callbacks.ctx.closed;
        }
        
    }
    
    mutex_unlock(s->mutex);
    
    if (!should_close)
        return;
    
    if (socket_fd >= 0)
        close(socket_fd);
    
    if (accept_thread != NULL)
        thread_destroy(accept_thread);
    
    if (receive_thread != NULL)
        thread_destroy(receive_thread);
    
    if (closed_callback != NULL)
        closed_callback(s, closed_callback_ctx);
    
    mutex_lock(s->mutex);
    
    s->close_in_progress = false;
    bool should_destroy = s->destroy_pending && !s->destroying &&
                          !s->accept_loop_active && !s->receive_loop_active;
    if (should_destroy)
        s->destroy_pending = false;
    
    mutex_unlock(s->mutex);
    
    if (should_destroy)
        socket_destroy(s);
    
}

struct sockaddr* socket_get_local_end_point(struct socket_t* s) {
    
    if (s == NULL)
        return NULL;
    
    mutex_lock(s->mutex);
    
    if (s->local_end_point == NULL && s->socket >= 0) {
        
        struct sockaddr_storage* addr = (struct sockaddr_storage*)malloc(sizeof(struct sockaddr_storage));
        if (addr != NULL) {
            socklen_t len = sizeof(struct sockaddr_storage);
            bzero(addr, len);
            if (getsockname(s->socket, (struct sockaddr*)addr, &len) == 0)
                s->local_end_point = (struct sockaddr*)addr;
            else
                free(addr);
        }
        
    }
    
    struct sockaddr* ret = s->local_end_point;
    
    mutex_unlock(s->mutex);
    
    return ret;
    
}

struct sockaddr* socket_get_remote_end_point(struct socket_t* s) {
    
    if (s == NULL)
        return NULL;
    
    mutex_lock(s->mutex);
    
    if (s->remote_end_point == NULL && s->socket >= 0) {
        
        struct sockaddr_storage* addr = (struct sockaddr_storage*)malloc(sizeof(struct sockaddr_storage));
        if (addr != NULL) {
            socklen_t len = sizeof(struct sockaddr_storage);
            if (getpeername(s->socket, (struct sockaddr*)addr, &len) == 0)
                s->remote_end_point = (struct sockaddr*)addr;
            else
                free(addr);
        }
        
    }
    
    struct sockaddr* ret = s->remote_end_point;
    
    mutex_unlock(s->mutex);
    
    return ret;
    
}

bool socket_is_udp(struct socket_t* s) {
    
    return s != NULL ? s->is_udp : false;
    
}

bool socket_is_connected(struct socket_t* s) {
    
    if (s == NULL)
        return false;
    
    mutex_lock(s->mutex);
    bool ret = s->is_connected;
    mutex_unlock(s->mutex);
    
    return ret;
    
}
