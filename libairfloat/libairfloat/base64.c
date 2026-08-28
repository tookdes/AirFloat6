//
//  base64.c
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

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "base64.h"

static char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int pos(char c) {
    char *p;
    for (p = base64_chars; *p; p++)
        if (*p == c)
            return (int)(p - base64_chars);
    return -1;
}

size_t base64_encode(const void *data, size_t size, char **str) {
    
    if (data == NULL || str == NULL)
        return (size_t)-1;
    
    *str = NULL;
    if (size > SIZE_MAX - 2)
        return (size_t)-1;
    size_t groups = (size + 2) / 3;
    if (groups > (SIZE_MAX - 1) / 4)
        return (size_t)-1;
    size_t encoded_capacity = groups * 4 + 1;
    
    char *s, *p;
    size_t i;
    int c;
    const unsigned char *q;
    
    p = s = (char *)malloc(encoded_capacity);
    if (p == NULL)
        return (size_t)-1;
    q = (const unsigned char *)data;
    for (i = 0; i < size;) {
        c = q[i++];
        c *= 256;
        if (i < size)
            c += q[i];
        i++;
        c *= 256;
        if (i < size)
            c += q[i];
        i++;
        p[0] = base64_chars[(c & 0x00fc0000) >> 18];
        p[1] = base64_chars[(c & 0x0003f000) >> 12];
        p[2] = base64_chars[(c & 0x00000fc0) >> 6];
        p[3] = base64_chars[(c & 0x0000003f) >> 0];
        if (i > size)
            p[3] = '=';
        if (i > size + 1)
            p[2] = '=';
        p += 4;
    }
    *p = '\0';
    *str = s;
    return (size_t)(p - s);
    
}

#define DECODE_ERROR 0xffffffff

static unsigned int token_decode(const char *token) {
    int i;
    unsigned int val = 0;
    int marker = 0;
    if (token == NULL || strlen(token) < 4)
        return DECODE_ERROR;
    for (i = 0; i < 4; i++) {
        val *= 64;
        if (token[i] == '=')
            marker++;
        else if (marker > 0)
            return DECODE_ERROR;
        else {
            int position = pos(token[i]);
            if (position < 0)
                return DECODE_ERROR;
            val += position;
        }
    }
    if (marker > 2)
        return DECODE_ERROR;
    return (marker << 24) | val;
}

size_t base64_decode(const char *str, void *data) {
    
    if (str == NULL || data == NULL)
        return (size_t)-1;
    
    size_t input_length = strlen(str);
    if (input_length == 0 || input_length % 4 != 0)
        return (size_t)-1;
    
    const char *p;
    unsigned char *q;
    
    q = (unsigned char*) data;
    for (p = str; *p; p += 4) {
        unsigned int val = token_decode(p);
        if (val == DECODE_ERROR)
            return (size_t)-1;
        
        unsigned int marker = (val >> 24) & 0xff;
        *q++ = (val >> 16) & 0xff;
        if (marker < 2)
            *q++ = (val >> 8) & 0xff;
        if (marker < 1)
            *q++ = val & 0xff;
        
        if (marker > 0 && p[4] != '\0')
            return (size_t)-1;
    }
    return (size_t)(q - (unsigned char *) data);
}

size_t base64_pad(const char* base64, size_t base64_size, char* out, size_t out_size) {
    
    if (base64 == NULL || out == NULL)
        return 0;
    
    size_t remainder = base64_size % 4;
    size_t padding = (4 - remainder) % 4;
    if (base64_size > SIZE_MAX - padding - 1)
        return 0;
    size_t required_size = base64_size + padding + 1;
    
    if (out_size < required_size)
        return 0;
    
    memcpy(out, base64, base64_size);
    for (size_t i = 0 ; i < padding ; i++)
        out[base64_size + i] = '=';
    out[base64_size + padding] = '\0';
    
    return base64_size + padding;
    
}
