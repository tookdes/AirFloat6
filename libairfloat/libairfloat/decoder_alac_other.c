//
//  decoder_alac_other.c
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

/* The legacy Xcode project only compiles this decoder wrapper. Its ordinary
   Debug configuration accidentally defines ALAC_SOFTWARE, while the explicit
   "Debug (Server Logs - ALAC Software)" configuration defines both
   ALAC_SOFTWARE and LOG_SERVER. Use Apple's bounded AudioConverter decoder for
   every normal iOS build, and retain the old software decoder only for that
   explicit diagnostic configuration and non-Apple platforms. */
#if defined(__APPLE__) && !(defined(ALAC_SOFTWARE) && defined(LOG_SERVER))

#ifdef ALAC_SOFTWARE
#undef ALAC_SOFTWARE
#endif
#include "decoder_alac_apple.c"

#else

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include "endian.h"
#include "alac.h"
#include "decoder.h"
#include "alac_format.h"

struct decoder_alac_other_t {
    struct alac_magic_cookie_t magic_cookie;
    struct decoder_output_format_t output_format;
    alac_file* alac;
};

static bool _decoder_alac_other_config_is_valid(struct alac_specific_config_t config) {
    uint32_t frame_length = btml(config.frame_length);
    uint32_t sample_rate = btml(config.sample_rate);
    return (frame_length > 0 && sample_rate > 0 && config.num_channels > 0 && config.num_channels <= 2 &&
            config.bit_depth >= 8 && config.bit_depth <= 32 && (config.bit_depth % 8) == 0);
}

static void _decoder_alac_other_release_codec(struct decoder_alac_other_t* d) {
    if (d == NULL || d->alac == NULL)
        return;
    deallocate_buffers(d->alac);
    dispose_alac(d->alac);
    d->alac = NULL;
}

static bool _decoder_alac_other_reset(struct decoder_alac_other_t* d) {
    if (d == NULL)
        return false;
    
    _decoder_alac_other_release_codec(d);
    
    struct alac_specific_config_t* config = &d->magic_cookie.alac_specific_info.config;
    if (!_decoder_alac_other_config_is_valid(*config))
        return false;
    
    d->alac = create_alac(config->bit_depth, config->num_channels);
    if (d->alac == NULL)
        return false;
    
    d->alac->setinfo_max_samples_per_frame = d->output_format.frames_per_packet = btml(config->frame_length);
    d->alac->setinfo_7a = config->compatible_version;
    d->alac->setinfo_sample_size = d->output_format.bit_depth = config->bit_depth;
    d->alac->setinfo_rice_historymult = config->pb;
    d->alac->setinfo_rice_initialhistory = config->mb;
    d->alac->setinfo_rice_kmodifier = config->kb;
    d->alac->setinfo_7f = d->output_format.channels = config->num_channels;
    d->alac->setinfo_80 = btms(config->max_run);
    d->alac->setinfo_82 = btml(config->max_frame_bytes);
    d->alac->setinfo_86 = btml(config->avg_bit_rate);
    d->alac->setinfo_8a_rate = d->output_format.sample_rate = btml(config->sample_rate);
    
    if (d->output_format.channels > UINT32_MAX / (d->output_format.bit_depth / 8)) {
        _decoder_alac_other_release_codec(d);
        return false;
    }
    d->output_format.frame_size = d->output_format.channels * (d->output_format.bit_depth / 8);
    if (d->output_format.frame_size == 0) {
        _decoder_alac_other_release_codec(d);
        return false;
    }
    
    allocate_buffers(d->alac);
    if (d->alac->predicterror_buffer_a == NULL || d->alac->predicterror_buffer_b == NULL ||
        d->alac->outputsamples_buffer_a == NULL || d->alac->outputsamples_buffer_b == NULL ||
        d->alac->uncompressed_bytes_buffer_a == NULL || d->alac->uncompressed_bytes_buffer_b == NULL) {
        _decoder_alac_other_release_codec(d);
        return false;
    }
    
    return true;
}

void* decoder_alac_create(const char* rtp_fmtp) {
    
    if (rtp_fmtp == NULL)
        return NULL;
    
    struct decoder_alac_other_t* d = (struct decoder_alac_other_t*)malloc(sizeof(struct decoder_alac_other_t));
    if (d == NULL)
        return NULL;
    bzero(d, sizeof(struct decoder_alac_other_t));
    
    d->magic_cookie = alac_format_parse(rtp_fmtp);
    if (!_decoder_alac_other_reset(d)) {
        free(d);
        return NULL;
    }
    
    return d;
}

void decoder_alac_destroy(void* data) {
    
    struct decoder_alac_other_t* d = (struct decoder_alac_other_t*)data;
    if (d == NULL)
        return;
    
    _decoder_alac_other_release_codec(d);
    free(d);
}

struct decoder_output_format_t decoder_alac_get_output_format(void* data) {
    
    struct decoder_alac_other_t* d = (struct decoder_alac_other_t*)data;
    return d != NULL ? d->output_format : (struct decoder_output_format_t){ 0, 0, 0, 0 };
}

size_t decoder_alac_decode(void* data, void* in_audio_data, size_t in_audio_data_size, void* out_audio_data, size_t out_audio_data_size) {
    
    struct decoder_alac_other_t* d = (struct decoder_alac_other_t*)data;
    if (d == NULL || d->alac == NULL || in_audio_data == NULL || out_audio_data == NULL ||
        in_audio_data_size == 0 || out_audio_data_size == 0 || out_audio_data_size > INT_MAX)
        return 0;
    
    int output_size = (int)out_audio_data_size;
    decode_frame(d->alac, (unsigned char*)in_audio_data, (unsigned char*)out_audio_data, &output_size);
    
    if (output_size < 0 || (size_t)output_size > out_audio_data_size)
        return 0;
    return (size_t)output_size;
}

void decoder_alac_reset(void* data) {
    
    struct decoder_alac_other_t* d = (struct decoder_alac_other_t*)data;
    if (d == NULL)
        return;
    
    if (!_decoder_alac_other_reset(d))
        bzero(&d->output_format, sizeof(d->output_format));
}

#endif
