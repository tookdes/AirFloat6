//
//  webheaders.c
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
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
//  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "webheaders.h"

struct web_header_t {
    char* name;
    char* value;
};

struct web_headers_t {
    struct web_header_t* headers;
    uint32_t count;
};

struct web_headers_t* web_headers_create() {
    
    struct web_headers_t* wh = (struct web_headers_t*)malloc(sizeof(struct web_headers_t));
    if (wh == NULL)
        return NULL;
    
    bzero(wh, sizeof(struct web_headers_t));
    
    return wh;
    
}

void web_headers_destroy(struct web_headers_t* wh) {
    
    if (wh == NULL)
        return;
    
    for (uint32_t i = 0 ; i < wh->count ; i++)
        free(wh->headers[i].name);
    
    free(wh->headers);
    free(wh);
    
}

struct web_headers_t* web_headers_copy(struct web_headers_t* wh) {
    
    if (wh == NULL)
        return NULL;
    
    struct web_headers_t* headers = web_headers_create();
    if (headers == NULL)
        return NULL;
    
    for (uint32_t i = 0 ; i < wh->count ; i++) {
        const char* name = wh->headers[i].name;
        const char* value = wh->headers[i].value;
        if (name == NULL || value == NULL)
            continue;
        
        size_t name_len = strlen(name);
        size_t value_len = strlen(value);
        char* storage = (char*)malloc(name_len + value_len + 2);
        if (storage == NULL) {
            web_headers_destroy(headers);
            return NULL;
        }
        
        struct web_header_t* new_headers = (struct web_header_t*)realloc(headers->headers, sizeof(struct web_header_t) * (headers->count + 1));
        if (new_headers == NULL) {
            free(storage);
            web_headers_destroy(headers);
            return NULL;
        }
        headers->headers = new_headers;
        
        struct web_header_t* new_header = &headers->headers[headers->count];
        new_header->name = storage;
        new_header->value = storage + name_len + 1;
        strcpy(new_header->name, name);
        strcpy(new_header->value, value);
        headers->count++;
    }
    
    return headers;
    
}

int32_t _web_headers_index_of(struct web_headers_t* wh, const char *name) {
    
    if (wh == NULL || name == NULL)
        return -1;
    
    for (uint32_t x = 0 ; x < wh->count ; x++)
        if (wh->headers[x].name != NULL && 0 == strcasecmp(wh->headers[x].name, name))
            return (int32_t)x;
    
    return -1;
    
}

const char* web_headers_value(struct web_headers_t* wh, const char *name) {
    
    int32_t index = _web_headers_index_of(wh, name);
    
    if (index > -1)
        return wh->headers[index].value;
    
    return NULL;
    
}

const char* web_headers_name(struct web_headers_t* wh, uint32_t index) {
    
    if (wh == NULL || index >= wh->count)
        return NULL;
    
    return wh->headers[index].name;
    
}

uint32_t web_headers_count(struct web_headers_t* wh) {
    
    return wh != NULL ? wh->count : 0;
    
}

size_t web_headers_parse(struct web_headers_t* wh, void* buffer, size_t size) {
    
    if (wh == NULL || buffer == NULL || size == 0)
        return 0;
    
    const char* read_buffer = (const char*)buffer;
    size_t line_start = 0;
    
    for (size_t i = 0 ; i < size ; i++) {
        if (read_buffer[i] != '\n')
            continue;
        
        size_t line_length = i - line_start;
        if (line_length == 0)
            return i + 1;
        
        const char* line = read_buffer + line_start;
        const char* separator = memchr(line, ':', line_length);
        if (separator != NULL && separator > line) {
            size_t name_len = (size_t)(separator - line);
            const char* value = separator + 1;
            const char* line_end = line + line_length;
            if (value < line_end && *value == ' ')
                value++;
            size_t value_len = (size_t)(line_end - value);
            
            char* storage = (char*)malloc(name_len + value_len + 2);
            if (storage == NULL)
                return SIZE_MAX;
            
            memcpy(storage, line, name_len);
            storage[name_len] = '\0';
            memcpy(storage + name_len + 1, value, value_len);
            storage[name_len + 1 + value_len] = '\0';
            
            struct web_header_t* headers = (struct web_header_t*)realloc(wh->headers, sizeof(struct web_header_t) * (wh->count + 1));
            if (headers == NULL) {
                free(storage);
                return SIZE_MAX;
            }
            wh->headers = headers;
            wh->headers[wh->count].name = storage;
            wh->headers[wh->count].value = storage + name_len + 1;
            wh->count++;
        }
        
        line_start = i + 1;
    }
    
    return size;
    
}

void web_headers_set_value(struct web_headers_t* wh, const char* name, const char* value, ...) {
    
    if (wh == NULL || name == NULL)
        return;
    
    char fvalue[1024];
    fvalue[0] = '\0';
    
    if (value != NULL) {
        va_list args;
        va_start(args, value);
        vsnprintf(fvalue, sizeof(fvalue), value, args);
        va_end(args);
    }
    
    int32_t index = _web_headers_index_of(wh, name);
    
    if (index > -1) {
        if (value == NULL) {
            free(wh->headers[index].name);
            for (uint32_t i = (uint32_t)index + 1 ; i < wh->count ; i++)
                wh->headers[i - 1] = wh->headers[i];
            wh->count--;
            return;
        }
        
        size_t name_len = strlen(name);
        size_t value_len = strlen(fvalue);
        char* storage = (char*)realloc(wh->headers[index].name, name_len + value_len + 2);
        if (storage == NULL)
            return;
        
        wh->headers[index].name = storage;
        wh->headers[index].value = storage + name_len + 1;
        strcpy(wh->headers[index].name, name);
        strcpy(wh->headers[index].value, fvalue);
        return;
    }
    
    if (value == NULL)
        return;
    
    size_t name_len = strlen(name);
    size_t value_len = strlen(fvalue);
    char* storage = (char*)malloc(name_len + value_len + 2);
    if (storage == NULL)
        return;
    
    struct web_header_t* headers = (struct web_header_t*)realloc(wh->headers, sizeof(struct web_header_t) * (wh->count + 1));
    if (headers == NULL) {
        free(storage);
        return;
    }
    wh->headers = headers;
    
    struct web_header_t* new_header = &wh->headers[wh->count];
    new_header->name = storage;
    new_header->value = storage + name_len + 1;
    strcpy(new_header->name, name);
    strcpy(new_header->value, fvalue);
    wh->count++;
    
}

size_t web_headers_write(struct web_headers_t* wh, void* data, size_t data_size) {
    
    if (wh == NULL)
        return 0;
    
    size_t write_pos = 0;
    
    for (uint32_t i = 0 ; i < wh->count ; i++) {
        if (wh->headers[i].name == NULL || wh->headers[i].value == NULL)
            continue;
        
        size_t name_len = strlen(wh->headers[i].name);
        size_t value_len = strlen(wh->headers[i].value);
        
        if (data != NULL) {
            if (write_pos + name_len + value_len + 4 > data_size)
                break;
            
            memcpy((char*)data + write_pos, wh->headers[i].name, name_len);
            write_pos += name_len;
            memcpy((char*)data + write_pos, ": ", 2);
            write_pos += 2;
            memcpy((char*)data + write_pos, wh->headers[i].value, value_len);
            write_pos += value_len;
            memcpy((char*)data + write_pos, "\r\n", 2);
            write_pos += 2;
        } else
            write_pos += name_len + value_len + 4;
    }
    
    if (data != NULL && write_pos + 2 <= data_size)
        memcpy((char*)data + write_pos, "\r\n", 2);
    
    write_pos += 2;
    
    return write_pos;
    
}
