//
//  raopserver.c
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
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "log.h"
#include "mutex.h"
#include "zeroconf.h"
#include "settings.h"
#include "webserver.h"
#include "raopsession.h"
#include "raopserver.h"

struct raop_server_t {
    mutex_p mutex;
    settings_p settings;
    web_server_p server;
    zeroconf_raop_ad_p zeroconf_ad;
    bool is_running;
    raop_session_p* sessions;
    uint32_t sessions_count;
    raop_server_new_session_callback new_session_callback;
    void* new_session_ctx;
    raop_server_accept_callback session_accept_callback;
    void* session_accept_callback_ctx;
};

bool _raop_server_web_connection_accept_callback(web_server_p server, web_server_connection_p connection, void* ctx) {
    
    struct raop_server_t* rs = (struct raop_server_t*)ctx;
    if (rs == NULL || connection == NULL)
        return false;
    
    raop_server_accept_callback accept_callback = NULL;
    void* accept_callback_ctx = NULL;
    
    mutex_lock(rs->mutex);
    accept_callback = rs->session_accept_callback;
    accept_callback_ctx = rs->session_accept_callback_ctx;
    mutex_unlock(rs->mutex);
    
    if (accept_callback != NULL) {
        const char *ip = web_server_connection_get_host(connection);
        uint16_t port = web_server_connection_get_port(connection);
        bool is_access_allowed = accept_callback(rs, ip, port, accept_callback_ctx);
        if (!is_access_allowed) {
            log_message(LOG_INFO, "Connection refused");
            return false;
        }
    }
    
#if (!defined(ALLOW_LOCALHOST))
    struct sockaddr* local_end_point = web_server_connection_get_local_end_point(connection);
    struct sockaddr* remote_end_point = web_server_connection_get_remote_end_point(connection);
    if (local_end_point != NULL && remote_end_point != NULL && !sockaddr_equals_host(local_end_point, remote_end_point)) {
#endif
        raop_session_p new_session = raop_session_create(rs, connection, rs->settings);
        if (new_session == NULL) {
            log_message(LOG_ERROR, "Unable to create RAOP session");
            return false;
        }
        
        mutex_lock(rs->mutex);
        raop_session_p* sessions = (raop_session_p*)realloc(rs->sessions, sizeof(raop_session_p) * (rs->sessions_count + 1));
        if (sessions == NULL) {
            mutex_unlock(rs->mutex);
            raop_session_destroy(new_session);
            log_message(LOG_ERROR, "Unable to store RAOP session");
            return false;
        }
        
        rs->sessions = sessions;
        rs->sessions[rs->sessions_count++] = new_session;
        raop_server_new_session_callback new_session_callback = rs->new_session_callback;
        void* new_session_ctx = rs->new_session_ctx;
        mutex_unlock(rs->mutex);
        
        raop_session_start(new_session);
        
        if (new_session_callback != NULL)
            new_session_callback(rs, new_session, new_session_ctx);
        
        return true;
#if (!defined(ALLOW_LOCALHOST))
    }
    return false;
#endif
}

struct raop_server_t* raop_server_create(struct raop_server_settings_t settings) {
    
    struct raop_server_t* rs = (struct raop_server_t*)malloc(sizeof(struct raop_server_t));
    if (rs == NULL)
        return NULL;
    bzero(rs, sizeof(struct raop_server_t));
    
    rs->settings = settings_create(settings.name, settings.password);
    if (rs->settings == NULL) {
        free(rs);
        return NULL;
    }
    
    rs->mutex = mutex_create();
    if (rs->mutex == NULL) {
        settings_destroy(rs->settings);
        free(rs);
        return NULL;
    }
    
    rs->server = web_server_create((sockaddr_type)(sockaddr_type_inet_4 | sockaddr_type_inet_6));
    if (rs->server == NULL) {
        mutex_destroy(rs->mutex);
        settings_destroy(rs->settings);
        free(rs);
        return NULL;
    }
    web_server_set_accept_callback(rs->server, _raop_server_web_connection_accept_callback, rs);
    
    return rs;
}

void raop_server_destroy(struct raop_server_t* rs) {
    
    if (rs == NULL)
        return;
    
    raop_server_stop(rs);
    web_server_destroy(rs->server);
    settings_destroy(rs->settings);
    mutex_destroy(rs->mutex);
    free(rs);
}

bool raop_server_start(struct raop_server_t* rs, uint16_t port) {
    
    if (rs == NULL || port == 0)
        return false;
    
    mutex_lock(rs->mutex);
    if (rs->is_running) {
        mutex_unlock(rs->mutex);
        return false;
    }
    
    bool web_started = web_server_start(rs->server, port);
    if (!web_started) {
        mutex_unlock(rs->mutex);
        log_message(LOG_INFO, "Unable to start server at port %d", port);
        return false;
    }
    
    zeroconf_raop_ad_p zeroconf_ad = zeroconf_raop_ad_create(port, settings_get_name(rs->settings));
    if (zeroconf_ad != NULL) {
        rs->zeroconf_ad = zeroconf_ad;
        rs->is_running = true;
    }
    mutex_unlock(rs->mutex);
    
    if (zeroconf_ad == NULL) {
        web_server_stop(rs->server);
        log_message(LOG_ERROR, "Unable to advertise RAOP server on port %d", port);
        return false;
    }
    
    log_message(LOG_INFO, "Server started at port %d", port);
    return true;
}

bool raop_server_is_running(struct raop_server_t* rs) {
    
    if (rs == NULL)
        return false;
    mutex_lock(rs->mutex);
    bool ret = rs->is_running;
    mutex_unlock(rs->mutex);
    return ret;
}

bool raop_server_is_recording(struct raop_server_t* rs) {
    
    if (rs == NULL)
        return false;
    
    bool ret = false;
    mutex_lock(rs->mutex);
    for (uint32_t i = 0 ; i < rs->sessions_count ; i++) {
        if (raop_session_is_recording(rs->sessions[i])) {
            ret = true;
            break;
        }
    }
    mutex_unlock(rs->mutex);
    return ret;
}

struct raop_server_settings_t raop_server_get_settings(struct raop_server_t* rs) {
    
    if (rs == NULL)
        return (struct raop_server_settings_t){ NULL, NULL };
    return (struct raop_server_settings_t){ settings_get_name(rs->settings), settings_get_password(rs->settings) };
}

void raop_server_set_settings(struct raop_server_t* rs, struct raop_server_settings_t settings) {
    
    if (rs == NULL)
        return;
    
    const char* requested_name = (settings.name != NULL && settings.name[0] != '\0') ? settings.name : "AirFloat";
    
    mutex_lock(rs->mutex);
    const char* old_name = settings_get_name(rs->settings);
    bool name_changed = (old_name == NULL || strcmp(old_name, requested_name) != 0);
    
    settings_set_name(rs->settings, settings.name);
    settings_set_password(rs->settings, settings.password);
    
    if (name_changed && rs->is_running) {
        struct sockaddr* local_end_point = web_server_get_local_end_point(rs->server, sockaddr_type_inet_4);
        uint16_t port = local_end_point != NULL ? sockaddr_get_port(local_end_point) : 0;
        
        if (rs->zeroconf_ad != NULL) {
            zeroconf_raop_ad_destroy(rs->zeroconf_ad);
            rs->zeroconf_ad = NULL;
        }
        
        if (port != 0)
            rs->zeroconf_ad = zeroconf_raop_ad_create(port, settings_get_name(rs->settings));
        
        if (rs->zeroconf_ad == NULL)
            log_message(LOG_ERROR, "Unable to refresh RAOP advertisement after settings change");
    }
    
    mutex_unlock(rs->mutex);
}

void raop_server_stop(struct raop_server_t* rs) {
    
    if (rs == NULL)
        return;
    
    mutex_lock(rs->mutex);
    bool was_running = rs->is_running;
    rs->is_running = false;
    zeroconf_raop_ad_p zeroconf_ad = rs->zeroconf_ad;
    rs->zeroconf_ad = NULL;
    mutex_unlock(rs->mutex);
    
    if (zeroconf_ad != NULL)
        zeroconf_raop_ad_destroy(zeroconf_ad);
    
    if (was_running)
        web_server_stop(rs->server);
    
    while (true) {
        mutex_lock(rs->mutex);
        if (rs->sessions_count == 0) {
            free(rs->sessions);
            rs->sessions = NULL;
            mutex_unlock(rs->mutex);
            break;
        }
        
        raop_session_p session = rs->sessions[0];
        for (uint32_t i = 1 ; i < rs->sessions_count ; i++)
            rs->sessions[i - 1] = rs->sessions[i];
        rs->sessions_count--;
        mutex_unlock(rs->mutex);
        
        raop_session_destroy(session);
    }
}

void raop_server_set_new_session_callback(struct raop_server_t* rs, raop_server_new_session_callback new_session_callback, void* ctx) {
    
    if (rs == NULL)
        return;
    mutex_lock(rs->mutex);
    rs->new_session_callback = new_session_callback;
    rs->new_session_ctx = ctx;
    mutex_unlock(rs->mutex);
}

void raop_server_set_session_accept_callback(struct raop_server_t* rs, raop_server_accept_callback session_accept_callback, void* ctx) {
    
    if (rs == NULL)
        return;
    mutex_lock(rs->mutex);
    rs->session_accept_callback = session_accept_callback;
    rs->session_accept_callback_ctx = ctx;
    mutex_unlock(rs->mutex);
}

void raop_server_session_ended(struct raop_server_t* rs, raop_session_p session) {
    
    if (rs == NULL || session == NULL)
        return;
    
    raop_session_p ended_session = NULL;
    mutex_lock(rs->mutex);
    for (uint32_t i = 0 ; i < rs->sessions_count ; i++) {
        if (rs->sessions[i] == session) {
            ended_session = rs->sessions[i];
            for (uint32_t a = i + 1 ; a < rs->sessions_count ; a++)
                rs->sessions[a - 1] = rs->sessions[a];
            rs->sessions_count--;
            break;
        }
    }
    mutex_unlock(rs->mutex);
    
    if (ended_session != NULL)
        raop_session_destroy(ended_session);
}

void raop_server_set_volume(struct raop_server_t* rs, float volume) {
    
    if (rs == NULL)
        return;
    
    mutex_lock(rs->mutex);
    for (uint32_t i = 0 ; i < rs->sessions_count ; i++) {
        if (raop_session_is_recording(rs->sessions[i]))
            raop_session_set_volume(rs->sessions[i], volume);
    }
    mutex_unlock(rs->mutex);
}
