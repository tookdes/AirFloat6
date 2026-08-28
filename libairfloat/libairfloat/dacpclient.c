//
//  dacpclient.c
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
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
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

#include "log.h"
#include "mutex.h"
#include "condition.h"
#include "zeroconf.h"
#include "dmap.h"
#include "sockaddr.h"
#include "webrequest.h"
#include "webresponse.h"
#include "webclientconnection.h"

#include "dacpclient.h"

#define DACP_MAX_STATUS_RESPONSE_SIZE (1024 * 1024)

struct dacp_client_t {
    mutex_p mutex;
    condition_p connection_condition;
    struct sockaddr* end_point;
    char* identifier;
    char* active_remove;
    zeroconf_dacp_discover_p dacp_discover;
    web_client_connection_p web_connection;
    web_client_connection_p pending_connection_destroy;
    uint32_t active_connection_users;
    uint32_t active_callbacks;
    struct {
        dacp_client_controls_became_available_callback controls_became_available;
        dacp_client_controls_became_unavailable_callback controls_became_unavailable;
        dacp_client_playback_state_changed_callback playback_state_changed;
        struct {
            void* controls_became_available;
            void* controls_became_unavailable;
            void* playback_state_changed;
        } ctx;
    } callbacks;
    enum dacp_client_playback_state playback_state;
    bool is_destroyed;
};

static bool _dacp_client_begin_callback(struct dacp_client_t* dc) {
    
    if (dc == NULL || dc->mutex == NULL)
        return false;
    
    mutex_lock(dc->mutex);
    dc->active_callbacks++;
    mutex_unlock(dc->mutex);
    return true;
}

static void _dacp_client_end_callback(struct dacp_client_t* dc) {
    
    if (dc == NULL || dc->mutex == NULL)
        return;
    
    mutex_lock(dc->mutex);
    if (dc->active_callbacks > 0)
        dc->active_callbacks--;
    if (dc->active_callbacks == 0 && dc->connection_condition != NULL)
        condition_broadcast(dc->connection_condition);
    mutex_unlock(dc->mutex);
}

static web_client_connection_p _dacp_client_acquire_connection(struct dacp_client_t* dc) {
    
    if (dc == NULL || dc->mutex == NULL)
        return NULL;
    
    web_client_connection_p connection = NULL;
    
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed && dc->web_connection != NULL) {
        dc->active_connection_users++;
        connection = dc->web_connection;
    }
    mutex_unlock(dc->mutex);
    
    return connection;
}

static void _dacp_client_release_connection(struct dacp_client_t* dc) {
    
    if (dc == NULL || dc->mutex == NULL)
        return;
    
    web_client_connection_p destroy_connection = NULL;
    
    mutex_lock(dc->mutex);
    if (dc->active_connection_users > 0)
        dc->active_connection_users--;
    
    if (dc->active_connection_users == 0) {
        /* During dacp_client_destroy(), leave the pending connection for the
           owner thread. It must join/destroy the web client before dc itself
           is freed, otherwise a callback worker could still hold dc as ctx. */
        if (!dc->is_destroyed) {
            destroy_connection = dc->pending_connection_destroy;
            dc->pending_connection_destroy = NULL;
        }
        if (dc->connection_condition != NULL)
            condition_broadcast(dc->connection_condition);
    }
    mutex_unlock(dc->mutex);
    
    /* Explicit web-client destruction suppresses its socket-closed callback,
       so no DACP callback can re-enter while this pending object is freed. */
    if (destroy_connection != NULL)
        web_client_connection_destroy(destroy_connection);
}

static web_client_connection_p _dacp_client_detach_connection(struct dacp_client_t* dc, web_client_connection_p connection) {
    
    if (dc == NULL || connection == NULL || dc->mutex == NULL)
        return NULL;
    
    web_client_connection_p destroy_connection = NULL;
    
    mutex_lock(dc->mutex);
    if (dc->web_connection == connection) {
        dc->web_connection = NULL;
        if (dc->active_connection_users > 0) {
            if (dc->pending_connection_destroy == NULL)
                dc->pending_connection_destroy = connection;
        } else
            destroy_connection = connection;
    }
    mutex_unlock(dc->mutex);
    
    return destroy_connection;
}

void _dacp_client_web_connection_connected_callback(web_client_connection_p connection, void* ctx) {
    
    struct dacp_client_t* dc = (struct dacp_client_t*)ctx;
    if (!_dacp_client_begin_callback(dc))
        return;
    
    dacp_client_controls_became_available_callback callback = NULL;
    void* callback_ctx = NULL;
    
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed && dc->web_connection == connection) {
        callback = dc->callbacks.controls_became_available;
        callback_ctx = dc->callbacks.ctx.controls_became_available;
    }
    mutex_unlock(dc->mutex);
    
    if (callback != NULL) {
        log_message(LOG_INFO, "Connected!");
        callback(dc, callback_ctx);
    }
    
    _dacp_client_end_callback(dc);
}

void _dacp_client_web_connection_connect_failed_callback(web_client_connection_p connection, void* ctx) {
    
    struct dacp_client_t* dc = (struct dacp_client_t*)ctx;
    if (!_dacp_client_begin_callback(dc))
        return;
    
    web_client_connection_p destroy_connection = _dacp_client_detach_connection(dc, connection);
    if (destroy_connection != NULL)
        web_client_connection_destroy(destroy_connection);
    
    _dacp_client_end_callback(dc);
}

void _dacp_client_web_connection_disconnected_callback(web_client_connection_p connection, void* ctx) {
    
    struct dacp_client_t* dc = (struct dacp_client_t*)ctx;
    if (!_dacp_client_begin_callback(dc))
        return;
    
    web_client_connection_p destroy_connection = _dacp_client_detach_connection(dc, connection);
    dacp_client_controls_became_unavailable_callback callback = NULL;
    void* callback_ctx = NULL;
    
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed) {
        callback = dc->callbacks.controls_became_unavailable;
        callback_ctx = dc->callbacks.ctx.controls_became_unavailable;
    }
    mutex_unlock(dc->mutex);
    
    if (destroy_connection != NULL)
        web_client_connection_destroy(destroy_connection);
    
    if (callback != NULL)
        callback(dc, callback_ctx);
    
    _dacp_client_end_callback(dc);
}

void _dacp_client_web_connection_response_received_callback(web_client_connection_p connection, web_request_p request, web_response_p response, void* ctx) {
    
    struct dacp_client_t* dc = (struct dacp_client_t*)ctx;
    if (!_dacp_client_begin_callback(dc))
        return;
    
    if (response == NULL) {
        _dacp_client_end_callback(dc);
        return;
    }
    
    mutex_lock(dc->mutex);
    bool valid_connection = !dc->is_destroyed && dc->web_connection == connection;
    mutex_unlock(dc->mutex);
    
    if (!valid_connection || web_response_get_status(response) != 200) {
        _dacp_client_end_callback(dc);
        return;
    }
    
    web_headers_p headers = web_response_get_headers(response);
    const char* s_content_type = web_headers_value(headers, "Content-Type");
    size_t content_length = web_response_get_content(response, NULL, 0);
    
    if (s_content_type == NULL ||
        strcmp(s_content_type, "application/x-dmap-tagged") != 0 ||
        content_length == 0 || content_length > DACP_MAX_STATUS_RESPONSE_SIZE) {
        _dacp_client_end_callback(dc);
        return;
    }
    
    char* data = (char*)malloc(content_length);
    if (data == NULL) {
        _dacp_client_end_callback(dc);
        return;
    }
    
    dacp_client_playback_state_changed_callback callback = NULL;
    void* callback_ctx = NULL;
    enum dacp_client_playback_state callback_state = dacp_client_playback_state_stopped;
    
    if (web_response_get_content(response, data, content_length) == content_length) {
        dmap_p dmap = dmap_create();
        if (dmap != NULL) {
            dmap_parse(dmap, data, content_length);
            
            dmap_p container = dmap_container_for_atom_identifer(dmap, "com.airfloat.nowplayingcontainer");
            if (container != NULL) {
                char now_playing = dmap_char_for_atom_identifer(container, "com.airfloat.nowplayingstatus");
                
                mutex_lock(dc->mutex);
                if (!dc->is_destroyed && dc->web_connection == connection) {
                    dc->playback_state = now_playing;
                    callback = dc->callbacks.playback_state_changed;
                    callback_ctx = dc->callbacks.ctx.playback_state_changed;
                    callback_state = now_playing;
                }
                mutex_unlock(dc->mutex);
            }
            
            dmap_destroy(dmap);
        }
    }
    
    free(data);
    
    if (callback != NULL)
        callback(dc, callback_state, callback_ctx);
    
    _dacp_client_end_callback(dc);
}

void _dacp_client_zeroconf_resolved_callback(zeroconf_dacp_discover_p zeroconf_dacp_discover, const char* name, struct sockaddr** end_points, uint32_t end_points_count, void* ctx) {
    
    struct dacp_client_t* dc = (struct dacp_client_t*)ctx;
    if (!_dacp_client_begin_callback(dc))
        return;
    
    if (name == NULL || end_points == NULL) {
        _dacp_client_end_callback(dc);
        return;
    }
    
    bool identifier_matches = false;
    
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed && dc->web_connection == NULL && dc->pending_connection_destroy == NULL &&
        dc->identifier != NULL && dc->end_point != NULL &&
        strlen(name) > 12 && memcmp(name, "iTunes_Ctrl_", 12) == 0 && strcmp(name + 12, dc->identifier) == 0)
        identifier_matches = true;
    mutex_unlock(dc->mutex);
    
    if (identifier_matches) {
        for (uint32_t i = 0 ; i < end_points_count ; i++) {
            
            if (end_points[i] == NULL)
                continue;
            
            mutex_lock(dc->mutex);
            bool host_matches = !dc->is_destroyed && dc->end_point != NULL && sockaddr_equals_host(end_points[i], dc->end_point);
            mutex_unlock(dc->mutex);
            
            if (!host_matches)
                continue;
            
            web_client_connection_p connection = web_client_connection_create();
            if (connection == NULL)
                break;
            
            web_client_connection_set_connected_callback(connection, _dacp_client_web_connection_connected_callback, dc);
            web_client_connection_set_connect_failed_callback(connection, _dacp_client_web_connection_connect_failed_callback, dc);
            web_client_connection_set_disconneced_callback(connection, _dacp_client_web_connection_disconnected_callback, dc);
            web_client_connection_set_response_received_callback(connection, _dacp_client_web_connection_response_received_callback, dc);
            
            bool installed = false;
            mutex_lock(dc->mutex);
            if (!dc->is_destroyed && dc->web_connection == NULL && dc->pending_connection_destroy == NULL) {
                dc->web_connection = connection;
                dc->active_connection_users++;
                installed = true;
            }
            mutex_unlock(dc->mutex);
            
            if (!installed) {
                web_client_connection_destroy(connection);
                break;
            }
            
            /* socket_connect may synchronously report setup failure. The active
               connection borrow keeps the web-client object alive until the
               callback has detached it and this call has returned. */
            web_client_connection_connect(connection, end_points[i]);
            _dacp_client_release_connection(dc);
            break;
        }
    }
    
    _dacp_client_end_callback(dc);
}

void _dacp_client_send_request(struct dacp_client_t* dc, const char* request_name) {
    
    if (dc == NULL || request_name == NULL)
        return;
    
    web_client_connection_p connection = _dacp_client_acquire_connection(dc);
    if (connection == NULL)
        return;
    
    if (!web_client_connection_is_connected(connection)) {
        _dacp_client_release_connection(dc);
        return;
    }
    
    char* active_remote = NULL;
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed && dc->active_remove != NULL) {
        size_t active_remote_length = strlen(dc->active_remove);
        active_remote = (char*)malloc(active_remote_length + 1);
        if (active_remote != NULL)
            memcpy(active_remote, dc->active_remove, active_remote_length + 1);
    }
    mutex_unlock(dc->mutex);
    
    web_request_p request = web_request_create();
    if (request == NULL) {
        free(active_remote);
        _dacp_client_release_connection(dc);
        return;
    }
    
    size_t request_name_length = strlen(request_name);
    if (request_name_length > SIZE_MAX - 13) {
        web_request_destroy(request);
        free(active_remote);
        _dacp_client_release_connection(dc);
        return;
    }
    
    size_t path_length = request_name_length + 13;
    char* path = (char*)malloc(path_length);
    if (path == NULL) {
        web_request_destroy(request);
        free(active_remote);
        _dacp_client_release_connection(dc);
        return;
    }
    snprintf(path, path_length, "/ctrl-int/1/%s", request_name);
    
    web_request_set_command(request, "GET");
    web_request_set_path(request, path);
    web_request_set_protocol(request, "HTTP/1.1");
    free(path);
    
    if (active_remote != NULL)
        web_headers_set_literal_value(web_request_get_headers(request), "Active-Remote", active_remote);
    
    web_client_connection_send_request(connection, request);
    web_request_destroy(request);
    free(active_remote);
    _dacp_client_release_connection(dc);
}

struct dacp_client_t* dacp_client_create(struct sockaddr* end_point, const char* identifier, const char* active_remote) {
    
    if (end_point == NULL || identifier == NULL || active_remote == NULL)
        return NULL;
    
    struct dacp_client_t* dc = (struct dacp_client_t*)malloc(sizeof(struct dacp_client_t));
    if (dc == NULL)
        return NULL;
    bzero(dc, sizeof(struct dacp_client_t));
    
    dc->mutex = mutex_create();
    dc->connection_condition = condition_create();
    if (dc->mutex == NULL || dc->connection_condition == NULL) {
        if (dc->connection_condition != NULL)
            condition_destroy(dc->connection_condition);
        if (dc->mutex != NULL)
            mutex_destroy(dc->mutex);
        free(dc);
        return NULL;
    }
    
    dc->end_point = sockaddr_copy(end_point);
    if (dc->end_point == NULL) {
        condition_destroy(dc->connection_condition);
        mutex_destroy(dc->mutex);
        free(dc);
        return NULL;
    }
    sockaddr_set_port(dc->end_point, 3689);
    
    dc->identifier = (char*)malloc(strlen(identifier) + 1);
    dc->active_remove = (char*)malloc(strlen(active_remote) + 1);
    if (dc->identifier == NULL || dc->active_remove == NULL) {
        free(dc->identifier);
        free(dc->active_remove);
        sockaddr_destroy(dc->end_point);
        condition_destroy(dc->connection_condition);
        mutex_destroy(dc->mutex);
        free(dc);
        return NULL;
    }
    strcpy(dc->identifier, identifier);
    strcpy(dc->active_remove, active_remote);
    dc->playback_state = dacp_client_playback_state_stopped;
    
    dc->dacp_discover = zeroconf_dacp_discover_create();
    if (dc->dacp_discover == NULL) {
        free(dc->identifier);
        free(dc->active_remove);
        sockaddr_destroy(dc->end_point);
        condition_destroy(dc->connection_condition);
        mutex_destroy(dc->mutex);
        free(dc);
        return NULL;
    }
    
    if (!zeroconf_dacp_discover_set_callback(dc->dacp_discover, _dacp_client_zeroconf_resolved_callback, dc)) {
        zeroconf_dacp_discover_destroy(dc->dacp_discover);
        dc->dacp_discover = NULL;
        free(dc->identifier);
        free(dc->active_remove);
        sockaddr_destroy(dc->end_point);
        condition_destroy(dc->connection_condition);
        mutex_destroy(dc->mutex);
        free(dc);
        return NULL;
    }
    
    return dc;
}

void dacp_client_destroy(struct dacp_client_t* dc) {
    
    if (dc == NULL || dc->mutex == NULL)
        return;
    
    zeroconf_dacp_discover_p discover = NULL;
    web_client_connection_p destroy_connection = NULL;
    
    mutex_lock(dc->mutex);
    if (dc->is_destroyed) {
        mutex_unlock(dc->mutex);
        return;
    }
    
    dc->is_destroyed = true;
    discover = dc->dacp_discover;
    dc->dacp_discover = NULL;
    
    if (dc->web_connection != NULL) {
        if (dc->active_connection_users > 0) {
            if (dc->pending_connection_destroy == NULL)
                dc->pending_connection_destroy = dc->web_connection;
        } else
            destroy_connection = dc->web_connection;
        dc->web_connection = NULL;
    }
    mutex_unlock(dc->mutex);
    
    /* Stop discovery first so no new connection callback can be introduced
       while the object is waiting for in-flight operations to return. */
    if (discover != NULL)
        zeroconf_dacp_discover_destroy(discover);
    
    mutex_lock(dc->mutex);
    while (dc->active_connection_users > 0 || dc->active_callbacks > 0)
        condition_wait(dc->connection_condition, dc->mutex);
    
    if (dc->pending_connection_destroy != NULL) {
        if (destroy_connection == NULL)
            destroy_connection = dc->pending_connection_destroy;
        dc->pending_connection_destroy = NULL;
    }
    mutex_unlock(dc->mutex);
    
    if (destroy_connection != NULL)
        web_client_connection_destroy(destroy_connection);
    
    if (dc->end_point != NULL)
        sockaddr_destroy(dc->end_point);
    free(dc->identifier);
    free(dc->active_remove);
    condition_destroy(dc->connection_condition);
    mutex_destroy(dc->mutex);
    free(dc);
}

void dacp_client_set_controls_became_available_callback(struct dacp_client_t* dc, dacp_client_controls_became_available_callback callback, void* ctx) {
    
    if (dc == NULL || dc->mutex == NULL)
        return;
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed) {
        dc->callbacks.controls_became_available = callback;
        dc->callbacks.ctx.controls_became_available = ctx;
    }
    mutex_unlock(dc->mutex);
}

void dacp_client_set_controls_became_unavailable_callback(struct dacp_client_t* dc, dacp_client_controls_became_unavailable_callback callback, void* ctx) {
    
    if (dc == NULL || dc->mutex == NULL)
        return;
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed) {
        dc->callbacks.controls_became_unavailable = callback;
        dc->callbacks.ctx.controls_became_unavailable = ctx;
    }
    mutex_unlock(dc->mutex);
}

void dacp_client_set_playback_state_changed_callback(struct dacp_client_t* dc, dacp_client_playback_state_changed_callback callback, void* ctx) {
    
    if (dc == NULL || dc->mutex == NULL)
        return;
    mutex_lock(dc->mutex);
    if (!dc->is_destroyed) {
        dc->callbacks.playback_state_changed = callback;
        dc->callbacks.ctx.playback_state_changed = ctx;
    }
    mutex_unlock(dc->mutex);
}

bool dacp_client_is_available(struct dacp_client_t* dc) {
    
    web_client_connection_p connection = _dacp_client_acquire_connection(dc);
    if (connection == NULL)
        return false;
    
    bool available = web_client_connection_is_connected(connection);
    _dacp_client_release_connection(dc);
    return available;
}

enum dacp_client_playback_state dacp_client_get_playback_state(dacp_client_p dc) {
    
    if (dc == NULL || dc->mutex == NULL)
        return dacp_client_playback_state_stopped;
    
    mutex_lock(dc->mutex);
    enum dacp_client_playback_state state = dc->playback_state;
    mutex_unlock(dc->mutex);
    return state;
}

void dacp_client_update_playback_state(struct dacp_client_t* dc) {
    
    _dacp_client_send_request(dc, "playstatusupdate");
}

void dacp_client_next(struct dacp_client_t* dc) {
    
    _dacp_client_send_request(dc, "nextitem");
}

void dacp_client_toggle_playback(struct dacp_client_t* dc) {
    
    _dacp_client_send_request(dc, "playpause");
}

void dacp_client_previous(struct dacp_client_t* dc) {
    
    _dacp_client_send_request(dc, "previtem");
}

void dacp_client_stop(struct dacp_client_t* dc) {
    
    _dacp_client_send_request(dc, "stop");
}

void dacp_client_seek(dacp_client_p dc, float seconds) {
    
    if (dc == NULL)
        return;
    
    uint32_t milliseconds = seconds > 0 ? (uint32_t)(seconds * 1000.0f) : 0;
    char cmd[100];
    snprintf(cmd, sizeof(cmd), "setproperty?dacp.playingtime=%u", milliseconds);
    _dacp_client_send_request(dc, cmd);
}

void dacp_client_set_volume(dacp_client_p dc, float volume) {
    
    if (dc != NULL && volume >= 0 && volume <= 100.0) {
        char cmd[100];
        snprintf(cmd, sizeof(cmd), "setproperty?dmcp.volume=%f", volume);
        _dacp_client_send_request(dc, cmd);
    }
}
