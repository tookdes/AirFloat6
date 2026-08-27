//
//  alac_format.c
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
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "endian.h"
#include "alac_format.h"

static bool _alac_format_parse_values(const char* rtp_fmtp, uint32_t values[11]) {
    
    if (rtp_fmtp == NULL || values == NULL)
        return false;
    
    const char* pos = rtp_fmtp;
    for (uint32_t i = 0 ; i < 11 ; i++) {
        while (*pos != '\0' && isspace((unsigned char)*pos))
            pos++;
        
        if (*pos == '\0' || *pos == '-')
            return false;
        
        errno = 0;
        char* end = NULL;
        unsigned long value = strtoul(pos, &end, 10);
        if (errno == ERANGE || end == pos || value > UINT32_MAX)
            return false;
        
        values[i] = (uint32_t)value;
        pos = end;
        
        if (i < 10) {
            if (*pos == '\0' || !isspace((unsigned char)*pos))
                return false;
        }
    }
    
    while (*pos != '\0' && isspace((unsigned char)*pos))
        pos++;
    if (*pos != '\0')
        return false;
    
    /* Keep validation broad enough for legacy ALAC variants while rejecting
       values that would overflow buffers or cannot fit the cookie fields. */
    if (values[0] == 0 || values[0] > 65536 ||
        values[1] > UINT8_MAX ||
        values[2] < 8 || values[2] > 32 || (values[2] % 8) != 0 ||
        values[3] > UINT8_MAX || values[4] > UINT8_MAX || values[5] > UINT8_MAX ||
        values[6] == 0 || values[6] > 8 ||
        values[7] > UINT16_MAX ||
        values[10] < 8000 || values[10] > 192000)
        return false;
    
    return true;
}

struct alac_magic_cookie_t alac_format_parse(const char* rtp_fmtp) {
    
    struct alac_magic_cookie_t cookie;
    bzero(&cookie, sizeof(struct alac_magic_cookie_t));
    
    uint32_t fmtp[11];
    if (!_alac_format_parse_values(rtp_fmtp, fmtp))
        return cookie;
    
    cookie.format_atom.atom_size = mtbl(12);
    cookie.format_atom.channel_layout_info_id = mtbl('frma');
    cookie.format_atom.type = mtbl('alac');
    cookie.alac_specific_info.info_size = mtbl(36);
    cookie.alac_specific_info.id = mtbl('alac');
    cookie.alac_specific_info.version_flag = 0;
    cookie.alac_specific_info.config.frame_length = mtbl(fmtp[0]);
    cookie.alac_specific_info.config.compatible_version = (uint8_t)fmtp[1];
    cookie.alac_specific_info.config.bit_depth = (uint8_t)fmtp[2];
    cookie.alac_specific_info.config.pb = (uint8_t)fmtp[3];
    cookie.alac_specific_info.config.mb = (uint8_t)fmtp[4];
    cookie.alac_specific_info.config.kb = (uint8_t)fmtp[5];
    cookie.alac_specific_info.config.num_channels = (uint8_t)fmtp[6];
    cookie.alac_specific_info.config.max_run = mtbs((uint16_t)fmtp[7]);
    cookie.alac_specific_info.config.max_frame_bytes = mtbl(fmtp[8]);
    cookie.alac_specific_info.config.avg_bit_rate = mtbl(fmtp[9]);
    cookie.alac_specific_info.config.sample_rate = mtbl(fmtp[10]);
    cookie.terminator_atom.channel_layout_info_size = mtbl(8);
    cookie.terminator_atom.channel_layout_info_id = 0;
    
    return cookie;
}
