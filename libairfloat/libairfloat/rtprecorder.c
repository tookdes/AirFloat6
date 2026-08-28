//
//  rtprecorder.c
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
#include <string.h>
#include <assert.h>
#include <math.h>

#include "crypt.h"
#include "mutex.h"
#include "log.h"
#include "condition.h"
#include "thread.h"
#include "hardware.h"
#include "rtpsocket.h"
#include "audioqueue.h"

#include "rtprecorder.h"

#define RTP_SOURCE                      0x0f
#define RTP_EXTENSION                   0x10
#define RTP_PAYLOAD_TYPE                0x7f
#define RTP_MARKER                      0x80

#define RTP_TIMING_REQUEST              0x52
#define RTP_TIMING_RESPONSE             0x53
#define RTP_SYNC                        0x54
#define RTP_RANGE_RESEND_REQUEST        0x55
#define RTP_AUDIO_RESEND_DATA           0x56
#define RTP_AUDIO_DATA                  0x60

#define NTP_UNIXEPOCH 0x83aa7e80
#define NTP_FRACTION 0xFFFFFFFF

struct ntp_time {
    uint32_t integer;
    uint32_t fraction;
};

static uint16_t _rtp_read_u16_be(const void* data) {
    uint16_t value = 0;
    memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

static uint32_t _rtp_read_u32_be(const void* data) {
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

static struct ntp_time _rtp_read_ntp(const void* data) {
    struct ntp_time value;
    memset(&value, 0, sizeof(value));
    memcpy(&value, data, sizeof(value));
    return value;
}

struct ntp_time _ntp_time_from_hardware_time(double time) {
    
    struct ntp_time ret;
    bzero(&ret, sizeof(struct ntp_time));
    
    double integer = floor(time);
    uint32_t i = (uint32_t)integer;
    i += NTP_UNIXEPOCH;
    ret.integer = htonl(i);
    ret.fraction = htonl((uint32_t)((time - integer) * (double)NTP_FRACTION));
    
    return ret;
}

double _ntp_time_to_hardware_time(struct ntp_time time) {
    
    uint32_t i = ntohl(time.integer);
    i -= NTP_UNIXEPOCH;
    return i + (ntohl(time.fraction) / (double)NTP_FRACTION);
}

struct rtp_packet_t {
    uint16_t seq_num;
    bool extension;
    uint8_t source;
    uint8_t payload_type;
    bool marker;
    const void* packet_data;
    size_t packet_data_size;
};

struct rtp_timing_packet_t {
    uint8_t a;
    uint8_t b;
    uint16_t seq_num;
    uint32_t _padding;
    struct ntp_time reference_time;
    struct ntp_time received_time;
    struct ntp_time send_time;
};

#define RTP_TIMING_PACKET_SIZE 32

struct rtp_resent_packet_t {
    uint8_t a;
    uint8_t b;
    uint16_t seq_num; /* Request sequence number */
    uint16_t missed_seq;
    uint16_t count;
};

#define RTP_RESEND_PACKET_SIZE 8

struct rtp_packet_t _rtp_header_read(const void* buffer, size_t size) {
    
    struct rtp_packet_t ret;
    memset(&ret, 0, sizeof(struct rtp_packet_t));
    
    if (buffer == NULL || size < 4)
        return ret;
    
    const uint8_t* bytes = (const uint8_t*)buffer;
    ret.seq_num = _rtp_read_u16_be(bytes + 2);
    ret.extension = (bool)(bytes[0] & RTP_EXTENSION);
    ret.source = bytes[0] & RTP_SOURCE;
    ret.payload_type = bytes[1] & RTP_PAYLOAD_TYPE;
    ret.marker = (bool)(bytes[1] & RTP_MARKER);
    ret.packet_data = bytes + 4;
    ret.packet_data_size = size - 4;
    
    return ret;
}

struct rtp_recorder_t {
    crypt_aes_p crypt;
    audio_queue_p audio_queue;
    mutex_p timer_mutex;
    condition_p timer_cond;
    thread_p synchronization_thread;
    uint32_t initial_time_response_count;
    rtp_socket_p streaming_socket;
    rtp_socket_p timing_socket;
    rtp_socket_p control_socket;
    struct sockaddr* remote_timing_end_point;
    struct sockaddr* remote_control_end_point;
    uint16_t emulated_seq_no;
    bool destroying;
    
    rtp_recorder_updated_track_position_callback updated_track_position_callback;
    void* updated_track_position_callback_ctx;
};

void _rtp_recorder_process_timing_packet(struct rtp_recorder_t* rr, struct rtp_packet_t* packet) {
    
    if (rr == NULL || packet == NULL || packet->packet_data == NULL || packet->packet_data_size < 28)
        return;
    
    const uint8_t* data = (const uint8_t*)packet->packet_data;
    double current_time = hardware_get_time();
    double reference_time = _ntp_time_to_hardware_time(_rtp_read_ntp(data + 4));
    double received_time = _ntp_time_to_hardware_time(_rtp_read_ntp(data + 12));
    double send_time = _ntp_time_to_hardware_time(_rtp_read_ntp(data + 20));
    
    double delay = ((current_time - reference_time) - (send_time - received_time)) / 2;
    double client_time = received_time + (send_time - received_time) + delay;
    
    log_message(LOG_INFO, "Client time is %1.6f (peer delay: %1.6f)", client_time, delay);
    audio_queue_set_remote_time(rr->audio_queue, client_time);
    
    mutex_lock(rr->timer_mutex);
    if (!rr->destroying) {
        rr->initial_time_response_count++;
        if (rr->initial_time_response_count >= 2)
            condition_signal(rr->timer_cond);
    }
    mutex_unlock(rr->timer_mutex);
}

void _rtp_recorder_send_timing_request(struct rtp_recorder_t* rr) {
    
    if (rr == NULL || rr->timing_socket == NULL || rr->remote_timing_end_point == NULL)
        return;
    
    struct rtp_timing_packet_t pckt;
    bzero(&pckt, sizeof(struct rtp_timing_packet_t));
    
    pckt.a = 0x80;
    pckt.b = RTP_TIMING_REQUEST | ~RTP_PAYLOAD_TYPE;
    pckt.seq_num = 0x0700;
    
    double send_time = hardware_get_time();
    pckt.send_time = _ntp_time_from_hardware_time(send_time);
    
    rtp_socket_send_to(rr->timing_socket, rr->remote_timing_end_point, &pckt, RTP_TIMING_PACKET_SIZE);
    log_message(LOG_INFO, "Timing synchronization request sent (@ %1.6f)", send_time);
}

void _rtp_recorder_synchronization_loop(void* ctx) {
    
    thread_set_name("Synchronization loop");
    
    struct rtp_recorder_t* rr = (struct rtp_recorder_t*)ctx;
    if (rr == NULL)
        return;
    
    mutex_lock(rr->timer_mutex);
    while (!rr->destroying && condition_times_wait(rr->timer_cond, rr->timer_mutex, 2000)) {
        if (!rr->destroying)
            _rtp_recorder_send_timing_request(rr);
    }
    mutex_unlock(rr->timer_mutex);
}

void _rtp_recorder_process_sync_packet(struct rtp_recorder_t* rr, struct rtp_packet_t* packet) {
    
    if (rr == NULL || packet == NULL || packet->packet_data == NULL || packet->packet_data_size < 16)
        return;
    
    const uint8_t* data = (const uint8_t*)packet->packet_data;
    uint32_t current_rtp_time = _rtp_read_u32_be(data);
    double current_time = _ntp_time_to_hardware_time(_rtp_read_ntp(data + 4));
    uint32_t next_rtp_time = _rtp_read_u32_be(data + 12);
    
    log_message(LOG_INFO, "Sync packet (Playhead frame: %u - current time: %1.6f - next frame: %u)", current_rtp_time, current_time, next_rtp_time);
    
    rtp_recorder_updated_track_position_callback callback = rr->updated_track_position_callback;
    void* callback_ctx = rr->updated_track_position_callback_ctx;
    if (callback != NULL)
        callback(rr, current_rtp_time, callback_ctx);
    
    audio_queue_synchronize(rr->audio_queue, current_rtp_time, current_time, next_rtp_time);
}

void _rtp_recorder_send_resend_request(struct rtp_recorder_t* rr, uint16_t seq_num, uint16_t count) {
    
    if (rr == NULL || count == 0 || rr->control_socket == NULL || rr->remote_control_end_point == NULL)
        return;
    
    struct rtp_resent_packet_t pckt;
    memset(&pckt, 0, sizeof(struct rtp_resent_packet_t));
    
    pckt.a = 0x80;
    pckt.b = RTP_RANGE_RESEND_REQUEST | ~RTP_PAYLOAD_TYPE;
    pckt.seq_num = htons(1);
    pckt.count = htons(count);
    pckt.missed_seq = htons(seq_num);
    
    rtp_socket_send_to(rr->control_socket, rr->remote_control_end_point, &pckt, RTP_RESEND_PACKET_SIZE);
    log_message(LOG_INFO, "Requested packet resend (seq: %d / count %d)", seq_num, count);
}

void _rtp_recorder_process_audio_packet(struct rtp_recorder_t* rr, struct rtp_packet_t* packet) {
    
    if (rr == NULL || packet == NULL || packet->packet_data == NULL || packet->packet_data_size <= 8)
        return;
    
    const uint8_t* packet_data = (const uint8_t*)packet->packet_data;
    uint32_t rtp_time = _rtp_read_u32_be(packet_data);
    uint16_t c_seq = packet->seq_num;
    
    const uint8_t* packet_audio_data = packet_data + 8;
    size_t len = packet->packet_data_size - 8;
    char* decoded_audio_data = (char*)malloc(len);
    if (decoded_audio_data == NULL)
        return;
    
    if (rr->crypt != NULL)
        len = crypt_aes_decrypt(rr->crypt, (void*)packet_audio_data, len, decoded_audio_data, len);
    else
        memcpy(decoded_audio_data, packet_audio_data, len);
    
    if (len > 0) {
        uint16_t missing_count = audio_queue_add_packet(rr->audio_queue, decoded_audio_data, len, c_seq, rtp_time);
        if (missing_count > 0)
            _rtp_recorder_send_resend_request(rr, (uint16_t)(c_seq - missing_count), missing_count);
    }
    
    free(decoded_audio_data);
    
    struct audio_queue_missing_packet_window next_missing_window = audio_queue_get_next_missing_window(rr->audio_queue);
    if (next_missing_window.packet_count > 0)
        _rtp_recorder_send_resend_request(rr, next_missing_window.seq_no, next_missing_window.packet_count);
}

size_t _rtp_recorder_socket_data_received_airtunes_v1(struct rtp_recorder_t* rr, rtp_socket_p rtp_socket, socket_p socket, const void* buffer, size_t size) {
    
    if (rr == NULL || buffer == NULL)
        return size;
    
    size_t read = 0;
    const uint8_t* bytes = (const uint8_t*)buffer;
    
    while (read < size) {
        if (size - read < 4)
            break;
        
        uint16_t packet_size = _rtp_read_u16_be(bytes + read + 2);
        if ((size_t)packet_size > size - read - 4)
            break;
        
        read += 4;
        
        if (packet_size >= 4 && bytes[read] == 0xf0 && bytes[read + 1] == 0xff) {
            struct rtp_packet_t packet = _rtp_header_read(bytes + read, packet_size);
            packet.seq_num = rr->emulated_seq_no++;
            if (packet.packet_data_size > 8)
                _rtp_recorder_process_audio_packet(rr, &packet);
        }
        
        read += packet_size;
    }
    
    return read;
}

size_t _rtp_recorder_socket_data_received_airtunes_v2(struct rtp_recorder_t* rr, rtp_socket_p rtp_socket, socket_p socket, const void* buffer, size_t size) {
    
    if (rr == NULL || buffer == NULL || size < 4)
        return size;
    
    struct rtp_packet_t packet = _rtp_header_read(buffer, size);
    
    switch (packet.payload_type) {
        case RTP_TIMING_RESPONSE:
            if (packet.packet_data_size >= 28)
                _rtp_recorder_process_timing_packet(rr, &packet);
            break;
        case RTP_SYNC:
            if (packet.packet_data_size >= 16)
                _rtp_recorder_process_sync_packet(rr, &packet);
            break;
        case RTP_AUDIO_RESEND_DATA:
            if (size >= 8) {
                packet = _rtp_header_read((const uint8_t*)buffer + 4, size - 4);
                if (packet.packet_data != NULL && packet.packet_data_size > 8) {
                    log_message(LOG_INFO, "Received missing packet %d", packet.seq_num);
                    _rtp_recorder_process_audio_packet(rr, &packet);
                }
            }
            break;
        case RTP_AUDIO_DATA:
            if (packet.packet_data_size > 8)
                _rtp_recorder_process_audio_packet(rr, &packet);
            break;
        default:
            log_message(LOG_ERROR, "Received unknown packet");
            break;
    }
    
    return size;
}

size_t _rtp_recorder_socket_data_received_callback(rtp_socket_p rtp_socket, socket_p socket, const void* buffer, size_t size, void* ctx) {
    
    struct rtp_recorder_t* rr = (struct rtp_recorder_t*)ctx;
    if (rr == NULL || socket == NULL)
        return size;
    
    if (socket_is_udp(socket))
        return _rtp_recorder_socket_data_received_airtunes_v2(rr, rtp_socket, socket, buffer, size);
    
    return _rtp_recorder_socket_data_received_airtunes_v1(rr, rtp_socket, socket, buffer, size);
}

rtp_socket_p _rtp_recorder_create_socket(struct rtp_recorder_t* rr, const char* name, struct sockaddr* local_end_point, struct sockaddr* remote_end_point) {
    
    if (rr == NULL || local_end_point == NULL || remote_end_point == NULL)
        return NULL;
    
    rtp_socket_p ret = rtp_socket_create(name, remote_end_point);
    if (ret == NULL)
        return NULL;
    
    for (unsigned short p = 6000 ; p < 6100 ; p++) {
        struct sockaddr* ep = sockaddr_copy(local_end_point);
        if (ep == NULL)
            break;
        sockaddr_set_port(ep, p);
        bool setup = rtp_socket_setup(ret, ep);
        sockaddr_destroy(ep);
        
        if (setup) {
            rtp_socket_set_data_received_callback(ret, _rtp_recorder_socket_data_received_callback, rr);
            log_message(LOG_INFO, "Setup socket on port %u", p);
            return ret;
        }
    }
    
    log_message(LOG_ERROR, "Unable to bind socket.");
    rtp_socket_destroy(ret);
    return NULL;
}

struct rtp_recorder_t* rtp_recorder_create(crypt_aes_p crypt, audio_queue_p audio_queue, struct sockaddr* local_end_point, struct sockaddr* remote_end_point, uint16_t remote_control_port, uint16_t remote_timing_port) {
    
    if (audio_queue == NULL || local_end_point == NULL || remote_end_point == NULL)
        return NULL;
    
    struct rtp_recorder_t* rr = (struct rtp_recorder_t*)malloc(sizeof(struct rtp_recorder_t));
    if (rr == NULL)
        return NULL;
    bzero(rr, sizeof(struct rtp_recorder_t));
    
    rr->crypt = crypt;
    rr->audio_queue = audio_queue;
    rr->timer_mutex = mutex_create();
    rr->timer_cond = condition_create();
    rr->remote_control_end_point = sockaddr_copy(remote_end_point);
    rr->remote_timing_end_point = sockaddr_copy(remote_end_point);
    
    if (rr->timer_mutex == NULL || rr->timer_cond == NULL ||
        rr->remote_control_end_point == NULL || rr->remote_timing_end_point == NULL) {
        rtp_recorder_destroy(rr);
        return NULL;
    }
    
    sockaddr_set_port(rr->remote_control_end_point, remote_control_port);
    sockaddr_set_port(rr->remote_timing_end_point, remote_timing_port);
    
    rr->streaming_socket = _rtp_recorder_create_socket(rr, "Streaming socket", local_end_point, remote_end_point);
    rr->control_socket = _rtp_recorder_create_socket(rr, "Control socket", local_end_point, remote_end_point);
    rr->timing_socket = _rtp_recorder_create_socket(rr, "Timing socket", local_end_point, remote_end_point);
    
    if (rr->streaming_socket == NULL || rr->control_socket == NULL || rr->timing_socket == NULL) {
        rtp_recorder_destroy(rr);
        return NULL;
    }
    
    return rr;
}

void rtp_recorder_destroy(struct rtp_recorder_t* rr) {
    
    if (rr == NULL)
        return;
    
    thread_p synchronization_thread = NULL;
    
    if (rr->timer_mutex != NULL) {
        mutex_lock(rr->timer_mutex);
        rr->destroying = true;
        synchronization_thread = rr->synchronization_thread;
        rr->synchronization_thread = NULL;
        if (rr->timer_cond != NULL)
            condition_broadcast(rr->timer_cond);
        mutex_unlock(rr->timer_mutex);
    } else
        rr->destroying = true;
    
    if (synchronization_thread != NULL)
        thread_destroy(synchronization_thread);
    
    if (rr->streaming_socket != NULL)
        rtp_socket_destroy(rr->streaming_socket);
    if (rr->control_socket != NULL)
        rtp_socket_destroy(rr->control_socket);
    if (rr->timing_socket != NULL)
        rtp_socket_destroy(rr->timing_socket);
    
    if (rr->remote_control_end_point != NULL)
        sockaddr_destroy(rr->remote_control_end_point);
    if (rr->remote_timing_end_point != NULL)
        sockaddr_destroy(rr->remote_timing_end_point);
    
    if (rr->timer_cond != NULL)
        condition_destroy(rr->timer_cond);
    if (rr->timer_mutex != NULL)
        mutex_destroy(rr->timer_mutex);
    
    free(rr);
}

bool rtp_recorder_start(struct rtp_recorder_t* rr) {
    
    if (rr == NULL || rr->timer_mutex == NULL || rr->timer_cond == NULL)
        return false;
    
    mutex_lock(rr->timer_mutex);
    if (rr->destroying) {
        mutex_unlock(rr->timer_mutex);
        return false;
    }
    
    bool complete = true;
    
    if (rr->initial_time_response_count < 2) {
        /* Keep the timer mutex locked while requests are sent. A very fast
           timing response then blocks until condition_times_wait atomically
           releases the mutex, so the wakeup cannot be lost. */
        _rtp_recorder_send_timing_request(rr);
        _rtp_recorder_send_timing_request(rr);
        _rtp_recorder_send_timing_request(rr);
        
        log_message(LOG_INFO, "Waiting for synchronization");
        
        while (!rr->destroying && rr->initial_time_response_count < 2) {
            if (condition_times_wait(rr->timer_cond, rr->timer_mutex, 5000)) {
                complete = false;
                break;
            }
        }
        
        if (!complete)
            log_message(LOG_INFO, "Initial time synchronization incomplete");
    }
    
    if (complete && !rr->destroying && rr->synchronization_thread == NULL) {
        rr->synchronization_thread = thread_create_a(_rtp_recorder_synchronization_loop, rr);
        if (rr->synchronization_thread == NULL) {
            complete = false;
            log_message(LOG_ERROR, "Unable to start synchronization thread");
        } else
            log_message(LOG_INFO, "Initial time synchronization complete");
    }
    
    mutex_unlock(rr->timer_mutex);
    return complete && !rr->destroying;
}

uint16_t rtp_recorder_get_streaming_port(struct rtp_recorder_t* rr) {
    
    if (rr == NULL || rr->streaming_socket == NULL)
        return 0;
    return rtp_socket_get_local_port(rr->streaming_socket);
}

uint16_t rtp_recorder_get_control_port(struct rtp_recorder_t* rr) {
    
    if (rr == NULL || rr->control_socket == NULL)
        return 0;
    return rtp_socket_get_local_port(rr->control_socket);
}

uint16_t rtp_recorder_get_timing_port(struct rtp_recorder_t* rr) {
    
    if (rr == NULL || rr->timing_socket == NULL)
        return 0;
    return rtp_socket_get_local_port(rr->timing_socket);
}

void rtp_recorder_set_updated_track_position_callback(struct rtp_recorder_t* rr, rtp_recorder_updated_track_position_callback callback, void* ctx) {
    if (rr == NULL)
        return;
    rr->updated_track_position_callback = callback;
    rr->updated_track_position_callback_ctx = ctx;
}
