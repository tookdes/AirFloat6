//
//  webrequest.c
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
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "log.h"

#include "webtools.h"
#include "webheaders.h"
#include "webresponse.h"
#include "webrequest.h"

#define MIN(a,b) ((a)<(b)?(a):(b))
#define WEB_REQUEST_MAX_HEADER_SIZE (64 * 1024)
#define WEB_REQUEST_MAX_CONTENT_SIZE (16 * 1024 * 1024)

struct web_request_t {
    char* command;
    char* path;
    char* protocol;
    void* content;
    size_t content_length;
    web_headers_p headers;
};

bool _web_request_parse_content_length(const char* value, size_t* content_length) {
    
    if (content_length == NULL)
        return false;
    
    *content_length = 0;
    if (value == NULL)
        return true;
    
    if (value[0] == '\0' || value[0] == '-')
        return false;
    
    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || end == NULL || *end != '\0' || parsed > WEB_REQUEST_MAX_CONTENT_SIZE)
        return false;
    
    *content_length = (size_t)parsed;
    return true;
    
}

struct web_request_t* web_request_create() {
    
    struct web_request_t* wr = (struct web_request_t*)malloc(sizeof(struct web_request_t));
    if (wr == NULL)
        return NULL;
    
    bzero(wr, sizeof(struct web_request_t));
    
    wr->headers = web_headers_create();
    if (wr->headers == NULL) {
        free(wr);
        return NULL;
    }
        
    return wr;
    
}

void web_request_destroy(struct web_request_t* wr) {
    
    if (wr == NULL)
        return;
    
    free(wr->command);
    free(wr->path);
    free(wr->protocol);
    free(wr->content);
    web_headers_destroy(wr->headers);
    free(wr);
    
}

ssize_t web_request_parse(struct web_request_t* wr, const void* data, size_t data_size) {
    
    if (wr == NULL || data == NULL || data_size == 0)
        return -1;
    
    if (data_size > WEB_REQUEST_MAX_HEADER_SIZE + WEB_REQUEST_MAX_CONTENT_SIZE)
        return -1;
    
    const char* content_start = web_tools_get_content_start(data, data_size);
    
    if (content_start == NULL) {
        if (data_size > WEB_REQUEST_MAX_HEADER_SIZE)
            return -1;
        return 0;
    }
    
    size_t header_length = (size_t)(content_start - (const char*)data);
    if (header_length == 0 || header_length > WEB_REQUEST_MAX_HEADER_SIZE)
        return -1;
    
    char* header = (char*)malloc(header_length + 1);
    if (header == NULL)
        return -1;
    
    memcpy(header, data, header_length);
    header[header_length] = '\0';
    
    log_data(LOG_INFO, data, header_length);
    
    header_length = web_tools_convert_new_lines(header, header_length);
    header[header_length] = '\0';
    
    char* first_line_end = (char*)memchr(header, '\n', header_length);
    if (first_line_end == NULL) {
        free(header);
        return -1;
    }
    *first_line_end = '\0';
    
    char* cmd = header;
    char* path = strchr(cmd, ' ');
    if (path == NULL) {
        free(header);
        return -1;
    }
    *path++ = '\0';
    while (*path == ' ')
        path++;
    
    char* protocol = strchr(path, ' ');
    if (protocol == NULL) {
        free(header);
        return -1;
    }
    *protocol++ = '\0';
    while (*protocol == ' ')
        protocol++;
    
    if (cmd[0] == '\0' || path[0] == '\0' || protocol[0] == '\0') {
        free(header);
        return -1;
    }
    
    char* headers_start = first_line_end + 1;
    size_t headers_length = header_length - (size_t)(headers_start - header);
    
    web_headers_p headers = web_headers_create();
    if (headers == NULL) {
        free(header);
        return -1;
    }
    web_headers_parse(headers, headers_start, headers_length);
    
    size_t content_length = 0;
    if (!_web_request_parse_content_length(web_headers_value(headers, "Content-Length"), &content_length)) {
        web_headers_destroy(headers);
        free(header);
        return -1;
    }
    
    size_t header_bytes = (size_t)(content_start - (const char*)data);
    size_t actual_content_length = data_size - header_bytes;
    if (content_length > actual_content_length) {
        log_message(LOG_INFO, "(Incomplete)");
        web_headers_destroy(headers);
        free(header);
        return 0;
    }
    
    char* new_command = (char*)malloc(strlen(cmd) + 1);
    char* new_path = (char*)malloc(strlen(path) + 1);
    char* new_protocol = (char*)malloc(strlen(protocol) + 1);
    if (new_command == NULL || new_path == NULL || new_protocol == NULL) {
        free(new_command);
        free(new_path);
        free(new_protocol);
        web_headers_destroy(headers);
        free(header);
        return -1;
    }
    strcpy(new_command, cmd);
    strcpy(new_path, path);
    strcpy(new_protocol, protocol);
    
    free(wr->command);
    free(wr->path);
    free(wr->protocol);
    wr->command = new_command;
    wr->path = new_path;
    wr->protocol = new_protocol;
    
    web_headers_destroy(wr->headers);
    wr->headers = headers;
    
    web_request_set_content(wr, content_start, content_length);
    if (content_length > 0 && wr->content == NULL) {
        free(header);
        return -1;
    }
    
    log_message(LOG_INFO, "(Complete) - %d bytes", content_length);
    
    size_t consumed = header_bytes + content_length;
    free(header);
    
    return (ssize_t)consumed;
    
}

struct web_request_t* web_request_copy(struct web_request_t* wr) {
    
    if (wr == NULL)
        return NULL;
    
    struct web_request_t* request = web_request_create();
    if (request == NULL)
        return NULL;
    
    web_request_set_command(request, wr->command);
    web_request_set_path(request, wr->path);
    web_request_set_protocol(request, wr->protocol);
    web_request_set_content(request, wr->content, wr->content_length);
    
    web_headers_p headers = web_headers_copy(wr->headers);
    if (headers == NULL) {
        web_request_destroy(request);
        return NULL;
    }
    web_headers_destroy(request->headers);
    request->headers = headers;
    
    return request;
    
}

void web_request_set_command(struct web_request_t* wr, const char* command) {
    
    if (wr == NULL)
        return;
    
    char* replacement = NULL;
    if (command != NULL) {
        replacement = (char*)malloc(strlen(command) + 1);
        if (replacement == NULL)
            return;
        strcpy(replacement, command);
    }
    
    free(wr->command);
    wr->command = replacement;
    
}

const char* web_request_get_command(struct web_request_t* wr) {
    return wr != NULL ? wr->command : NULL;
}

void web_request_set_path(struct web_request_t* wr, const char* path) {
    
    if (wr == NULL)
        return;
    
    char* replacement = NULL;
    if (path != NULL) {
        replacement = (char*)malloc(strlen(path) + 1);
        if (replacement == NULL)
            return;
        strcpy(replacement, path);
    }
    
    free(wr->path);
    wr->path = replacement;
    
}

const char* web_request_get_path(struct web_request_t* wr) {
    return wr != NULL ? wr->path : NULL;
}

void web_request_set_protocol(struct web_request_t* wr, const char* protocol) {
    
    if (wr == NULL)
        return;
    
    char* replacement = NULL;
    if (protocol != NULL) {
        replacement = (char*)malloc(strlen(protocol) + 1);
        if (replacement == NULL)
            return;
        strcpy(replacement, protocol);
    }
    
    free(wr->protocol);
    wr->protocol = replacement;
    
}

const char* web_request_get_protocol(struct web_request_t* wr) {
    return wr != NULL ? wr->protocol : NULL;
}

size_t web_request_get_content(struct web_request_t* wr, void* data, size_t data_size) {
    
    if (wr == NULL)
        return 0;
    
    if (data == NULL)
        return wr->content_length;
    
    if (wr->content != NULL) {
        size_t ret = MIN(wr->content_length, data_size);
        memcpy(data, wr->content, ret);
        return ret;
    }
    
    return 0;
    
}

void web_request_set_content(struct web_request_t* wr, const void* data, size_t data_size) {
    
    if (wr == NULL)
        return;
    
    void* replacement = NULL;
    if (data != NULL && data_size > 0) {
        replacement = malloc(data_size);
        if (replacement == NULL)
            return;
        memcpy(replacement, data, data_size);
    }
    
    free(wr->content);
    wr->content = replacement;
    wr->content_length = (replacement != NULL ? data_size : 0);
    
}

web_headers_p web_request_get_headers(struct web_request_t* wr) {
    return wr != NULL ? wr->headers : NULL;
}

size_t web_request_write(struct web_request_t* wr, void* data, size_t data_size) {
    
    if (wr == NULL || wr->command == NULL || wr->path == NULL || wr->protocol == NULL)
        return 0;
    
    size_t write_pos = 0;
    size_t head_len = strlen(wr->command) + strlen(wr->path) + strlen(wr->protocol) + 4;
    
    if (data != NULL && write_pos + head_len <= data_size)
        sprintf((char*)data, "%s %s %s\r\n", wr->command, wr->path, wr->protocol);
    
    write_pos += head_len;
    
    size_t headers_len = web_headers_write(wr->headers, NULL, 0);
    
    if (data != NULL && write_pos + headers_len <= data_size)
        web_headers_write(wr->headers, (char*)data + write_pos, data_size - write_pos);
    
    write_pos += headers_len;
    
    return write_pos;
    
}
