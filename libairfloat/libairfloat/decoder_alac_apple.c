//
//  decoder_alac_mac.c
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

#if (defined(__APPLE__) && !defined(ALAC_SOFTWARE))

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#include <AudioToolbox/AudioToolbox.h>

#include "log.h"
#include "endian.h"
#include "decoder.h"
#include "alac_format.h"

#define MIN(x, y) (x < y ? x : y)

struct decoder_alac_mac_t {
    struct alac_magic_cookie_t magic_cookie;
    struct decoder_output_format_t output_format;
    AudioStreamBasicDescription out_desc;
    AudioConverterRef converter_ref;
    AudioStreamPacketDescription packet_description;
    void* buffer_p;
    size_t buffer_size;
};

static bool _decoder_alac_config_is_valid(struct alac_specific_config_t config) {
    uint32_t frame_length = btml(config.frame_length);
    uint32_t sample_rate = btml(config.sample_rate);
    return (frame_length > 0 && sample_rate > 0 && config.num_channels > 0 &&
            config.bit_depth >= 8 && config.bit_depth <= 32 && (config.bit_depth % 8) == 0);
}

static void _decoder_alac_cleanup(struct decoder_alac_mac_t* d) {
    if (d == NULL)
        return;
    if (d->converter_ref != NULL) {
        OSStatus status = AudioConverterDispose(d->converter_ref);
        if (status != noErr)
            log_message(LOG_ERROR, "Unable to dispose ALAC converter (%d)", (int)status);
        d->converter_ref = NULL;
    }
}

void* decoder_alac_create(const char* rtp_fmtp) {
    
    if (rtp_fmtp == NULL)
        return NULL;
    
    struct decoder_alac_mac_t* d = (struct decoder_alac_mac_t*)malloc(sizeof(struct decoder_alac_mac_t));
    if (d == NULL)
        return NULL;
    bzero(d, sizeof(struct decoder_alac_mac_t));
    
    d->magic_cookie = alac_format_parse(rtp_fmtp);
    struct alac_specific_config_t config = d->magic_cookie.alac_specific_info.config;
    if (!_decoder_alac_config_is_valid(config)) {
        free(d);
        return NULL;
    }
    
    struct AudioStreamBasicDescription in_desc;
    bzero(&in_desc, sizeof(in_desc));
    
    in_desc.mFormatID = kAudioFormatAppleLossless;
    in_desc.mSampleRate = btml(config.sample_rate);
    in_desc.mFramesPerPacket = btml(config.frame_length);
    in_desc.mChannelsPerFrame = config.num_channels;
    
    d->out_desc.mFormatID = kAudioFormatLinearPCM;
    d->out_desc.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    d->out_desc.mSampleRate = in_desc.mSampleRate;
    d->out_desc.mChannelsPerFrame = config.num_channels;
    d->out_desc.mFramesPerPacket = 1;
    d->out_desc.mBitsPerChannel = config.bit_depth;
    
    UInt32 size = sizeof(AudioStreamBasicDescription);
    OSStatus status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &size, &d->out_desc);
    if (status != noErr || d->out_desc.mBytesPerFrame == 0) {
        log_message(LOG_ERROR, "Invalid ALAC output format (%d)", (int)status);
        free(d);
        return NULL;
    }
    
    d->output_format.frames_per_packet = in_desc.mFramesPerPacket;
    d->output_format.sample_rate = (uint32_t)d->out_desc.mSampleRate;
    d->output_format.channels = d->out_desc.mChannelsPerFrame;
    d->output_format.bit_depth = d->out_desc.mBitsPerChannel;
    d->output_format.frame_size = d->out_desc.mBytesPerFrame;
    
    status = AudioConverterNew(&in_desc, &d->out_desc, &d->converter_ref);
    if (status != noErr || d->converter_ref == NULL) {
        log_message(LOG_ERROR, "Unable to create ALAC converter (%d)", (int)status);
        free(d);
        return NULL;
    }
    
    AudioChannelLayout channel_layout;
    bzero(&channel_layout, sizeof(channel_layout));
    channel_layout.mChannelLayoutTag = (config.num_channels == 2 ? kAudioChannelLayoutTag_Stereo : kAudioChannelLayoutTag_Mono);
    
    status = AudioConverterSetProperty(d->converter_ref, kAudioConverterDecompressionMagicCookie, sizeof(struct alac_magic_cookie_t), &d->magic_cookie);
    if (status == noErr)
        status = AudioConverterSetProperty(d->converter_ref, kAudioConverterInputChannelLayout, sizeof(channel_layout), &channel_layout);
    
    if (status != noErr) {
        log_message(LOG_ERROR, "Unable to configure ALAC converter (%d)", (int)status);
        _decoder_alac_cleanup(d);
        free(d);
        return NULL;
    }
    
    return d;
}

void decoder_alac_destroy(void* data) {
    
    struct decoder_alac_mac_t* d = (struct decoder_alac_mac_t*)data;
    if (d == NULL)
        return;
    
    _decoder_alac_cleanup(d);
    free(d);
}

struct decoder_output_format_t decoder_alac_get_output_format(void* data) {
    
    struct decoder_alac_mac_t* d = (struct decoder_alac_mac_t*)data;
    return d != NULL ? d->output_format : (struct decoder_output_format_t){ 0, 0, 0, 0 };
}

OSStatus _decoder_alac_mac_input_data_proc(AudioConverterRef inAudioConverter, UInt32 *ioNumberDataPackets, AudioBufferList *ioData, AudioStreamPacketDescription **outDataPacketDescription, void *inUserData) {
    
    struct decoder_alac_mac_t* d = (struct decoder_alac_mac_t*)inUserData;
    if (d == NULL || ioNumberDataPackets == NULL || ioData == NULL || d->buffer_p == NULL || d->buffer_size == 0 || d->buffer_size > UINT32_MAX) {
        if (ioNumberDataPackets != NULL)
            *ioNumberDataPackets = 0;
        return kAudio_ParamError;
    }
    
    *ioNumberDataPackets = 1;
    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mData = d->buffer_p;
    ioData->mBuffers[0].mDataByteSize = (UInt32)d->buffer_size;
    ioData->mBuffers[0].mNumberChannels = d->magic_cookie.alac_specific_info.config.num_channels;
    
    bzero(&d->packet_description, sizeof(d->packet_description));
    d->packet_description.mDataByteSize = (UInt32)d->buffer_size;
    
    if (outDataPacketDescription != NULL)
        *outDataPacketDescription = &d->packet_description;
    
    return noErr;
}

size_t decoder_alac_decode(void* data, void* in_audio_data, size_t in_audio_data_size, void* out_audio_data, size_t out_audio_data_size) {
    
    struct decoder_alac_mac_t* d = (struct decoder_alac_mac_t*)data;
    if (d == NULL || d->converter_ref == NULL || in_audio_data == NULL || out_audio_data == NULL ||
        in_audio_data_size == 0 || out_audio_data_size == 0 ||
        in_audio_data_size > UINT32_MAX || out_audio_data_size > UINT32_MAX ||
        d->output_format.frame_size == 0)
        return 0;
    
    AudioBufferList out_buffer_list;
    bzero(&out_buffer_list, sizeof(out_buffer_list));
    out_buffer_list.mNumberBuffers = 1;
    out_buffer_list.mBuffers[0].mData = out_audio_data;
    out_buffer_list.mBuffers[0].mDataByteSize = (UInt32)out_audio_data_size;
    out_buffer_list.mBuffers[0].mNumberChannels = d->output_format.channels;
    
    d->buffer_size = in_audio_data_size;
    d->buffer_p = in_audio_data;
    
    UInt32 io_output_data_packets = (UInt32)(out_audio_data_size / d->output_format.frame_size);
    if (io_output_data_packets == 0) {
        d->buffer_p = NULL;
        d->buffer_size = 0;
        return 0;
    }
    
    OSStatus status = AudioConverterFillComplexBuffer(d->converter_ref, _decoder_alac_mac_input_data_proc, d, &io_output_data_packets, &out_buffer_list, NULL);
    d->buffer_p = NULL;
    d->buffer_size = 0;
    
    if (status != noErr) {
        log_message(LOG_ERROR, "ALAC decode failed (%d)", (int)status);
        return 0;
    }
    
    if (io_output_data_packets > SIZE_MAX / d->output_format.frame_size)
        return 0;
    size_t decoded_size = (size_t)io_output_data_packets * d->output_format.frame_size;
    return MIN(decoded_size, out_audio_data_size);
}

void decoder_alac_reset(void* data) {
    
    struct decoder_alac_mac_t* d = (struct decoder_alac_mac_t*)data;
    if (d == NULL || d->converter_ref == NULL)
        return;
    
    OSStatus status = AudioConverterReset(d->converter_ref);
    if (status != noErr)
        log_message(LOG_ERROR, "Unable to reset ALAC converter (%d)", (int)status);
}

#endif
