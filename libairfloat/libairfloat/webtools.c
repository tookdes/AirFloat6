//
//  webtools.c
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
#include "webheaders.h"
#include "webtools.h"

size_t web_tools_convert_new_lines(void* data, size_t data_size) {
    
    if (data == NULL || data_size == 0)
        return 0;
    
    char* bytes = (char*)data;
    
    for (size_t i = 0; i < data_size; i++) {
        if (bytes[i] == '\r') {
            if (i + 1 < data_size && bytes[i + 1] == '\n') {
                /* Remove the CR from CRLF. Source and destination overlap. */
                memmove(bytes + i, bytes + i + 1, data_size - (i + 1));
                data_size--;
                if (i > 0)
                    i--;
            } else
                bytes[i] = '\n';
        }
        
        if (i + 1 < data_size && bytes[i] == '\n' && bytes[i + 1] == '\n')
            break;
    }
    
    return data_size;
}

const char* web_tools_get_content_start(const void* data, size_t data_size) {
    
    if (data == NULL || data_size == 0)
        return NULL;
    
    const char* bytes = (const char*)data;
    
    for (size_t i = 0; i < data_size; i++) {
        if (i + 4 <= data_size && memcmp(bytes + i, "\r\n\r\n", 4) == 0)
            return bytes + i + 4;
        if (i + 2 <= data_size && memcmp(bytes + i, "\n\n", 2) == 0)
            return bytes + i + 2;
        if (i + 2 <= data_size && memcmp(bytes + i, "\r\r", 2) == 0)
            return bytes + i + 2;
    }
    
    return NULL;
}
