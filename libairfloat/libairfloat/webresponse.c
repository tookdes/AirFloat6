//
//  webresponse.c
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
#include <stdint.h>
#include <assert.h>
#include <errno.h>

#include "log.h"
#include "webtools.h"
#include "webheaders.h"
#include "webresponse.h"

#define MIN(a,b) ((a)<(b)?(a):(b))
#define WEB_RESPONSE_MAX_HEADER_SIZE (64 * 1024)
#define WEB_RESPONSE_MAX_CONTENT_SIZE (16 * 1024 * 1024)

struct web_response_t {
    uint16_t status_code;
    char* status_message;
    web_headers_p headers;
    void* content;
    size_t content_length;
    bool keep_alive;
};

bool _web_response_parse_content_length(const char* value, size_t* content_length) {
    
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
    if (errno == ERANGE || end == value || end == NULL || *end != '\0' || parsed > WEB_RESPONSE_MAX_CONTENT_SIZE)
        return false;
    
    *content_length = (size_t)parsed;
    return true;
    
}

bool _web_response_parse_status_code(const char* value, uint16_t* status_code) {
    
    if (value == NULL || status_code == NULL || value[0] == '\0')
        return false;
    
    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || end == NULL || *end != '\0' || parsed < 100 || parsed > 999)
        return false;
    
    *status_code = (uint16_t)parsed;
    return true;
    
}

struct web_response_t* web_response_create() {
    
    struct web_response_t* wr = (struct web_response_t*)malloc(sizeof(struct web_response_t));
    if (wr == NULL)
        return NULL;
    
    bzero(wr, sizeof(struct web_response_t));
    
    wr->headers = web_headers_create();
    if (wr->headers == NULL) {
        free(wr);
        return NULL;
    }
    
    web_response_set_status(wr, 500, "Internal Server Error");
    if (wr->status_message == NULL) {
        web_headers_destroy(wr->headers);
        free(wr);
        return NULL;
    }
    
    return wr;
    
}

void web_response_destroy(struct web_response_t* wr) {
    
    if (wr == NULL)
        return;
    
    free(wr->status_message);
    free(wr->content);
    web_headers_destroy(wr->headers);
    free(wr);
    
}

ssize_t web_response_parse(web_response_p wr, const void* data, size_t data_size) {
    
    if (wr == NULL || data == NULL || data_size == 0)
        return -1;
    
    if (data_size > WEB_RESPONSE_MAX_HEADER_SIZE + WEB_RESPONSE_MAX_CONTENT_SIZE)
        return -1;
    
    const char* content_start = web_tools_get_content_start(data, data_size);
    if (content_start == NULL) {
        if (data_size > WEB_RESPONSE_MAX_HEADER_SIZE)
            return -1;
        return 0;
    }
    
    size_t header_length = (size_t)(content_start - (const char*)data);
    if (header_length == 0 || header_length > WEB_RESPONSE_MAX_HEADER_SIZE)
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
    
    char* protocol = header;
    char* s_status_code = strchr(protocol, ' ');
    if (s_status_code == NULL) {
        free(header);
        return -1;
    }
    *s_status_code++ = '\0';
    while (*s_status_code == ' ')
        s_status_code++;
    
    char* status_message = strchr(s_status_code, ' ');
    if (status_message == NULL) {
        free(header);
        return -1;
    }
    *status_message++ = '\0';
    while (*status_message == ' ')
        status_message++;
    
    uint16_t status_code = 0;
    if (protocol[0] == '\0' || status_message[0] == '\0' || !_web_response_parse_status_code(s_status_code, &status_code)) {
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
    if (web_headers_parse(headers, headers_start, headers_length) == SIZE_MAX) {
        web_headers_destroy(headers);
        free(header);
        return -1;
    }
    
    size_t content_length = 0;
    if (!_web_response_parse_content_length(web_headers_value(headers, "Content-Length"), &content_length)) {
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
    
    char* new_status_message = (char*)malloc(strlen(status_message) + 1);
    if (new_status_message == NULL) {
        web_headers_destroy(headers);
        free(header);
        return -1;
    }
    strcpy(new_status_message, status_message);
    
    void* new_content = NULL;
    if (content_length > 0) {
        new_content = malloc(content_length);
        if (new_content == NULL) {
            free(new_status_message);
            web_headers_destroy(headers);
            free(header);
            return -1;
        }
        memcpy(new_content, content_start, content_length);
    }
    
    free(wr->status_message);
    wr->status_message = new_status_message;
    wr->status_code = status_code;
    
    web_headers_destroy(wr->headers);
    wr->headers = headers;
    
    free(wr->content);
    wr->content = new_content;
    wr->content_length = content_length;
    
    log_message(LOG_INFO, "(Complete) - %d bytes", content_length);
    
    size_t consumed = header_bytes + content_length;
    free(header);
    
    return (ssize_t)consumed;
    
}

web_headers_p web_response_get_headers(struct web_response_t* wr) {
    
    return wr != NULL ? wr->headers : NULL;
    
}

void web_response_set_status(struct web_response_t* wr, uint16_t code, const char* message) {
    
    if (wr == NULL || code >= 1000)
        return;
    
    if (message == NULL) {
        code = 500;
        message = "Internal Server Error";
    }
    
    char* replacement = (char*)malloc(strlen(message) + 1);
    if (replacement == NULL)
        return;
    strcpy(replacement, message);
    
    free(wr->status_message);
    wr->status_message = replacement;
    wr->status_code = code;
    
}

uint16_t web_response_get_status(struct web_response_t* wr) {
    
    return wr != NULL ? wr->status_code : 0;
    
}

const char* web_response_get_status_message(struct web_response_t* wr) {
    
    return wr != NULL ? wr->status_message : NULL;
    
}

void web_response_set_content(struct web_response_t* wr, void* content, size_t size) {
    
    if (wr == NULL)
        return;
    
    void* replacement = NULL;
    if (content != NULL && size > 0) {
        replacement = malloc(size);
        if (replacement == NULL)
            return;
        memcpy(replacement, content, size);
    }
    
    free(wr->content);
    wr->content = replacement;
    wr->content_length = (replacement != NULL ? size : 0);
    
    if (replacement != NULL)
        web_headers_set_value(wr->headers, "Content-Length", "%lu", (unsigned long)size);
    else
        web_headers_set_value(wr->headers, "Content-Length", NULL);
    
}

size_t web_response_get_content(struct web_response_t* wr, void* content, size_t size) {
    
    if (wr == NULL)
        return 0;
    
    if (content != NULL && wr->content != NULL)
        memcpy(content, wr->content, MIN(size, wr->content_length));
    
    return wr->content_length;
    
}

void web_response_set_keep_alive(struct web_response_t* wr, bool keep_alive) {
    
    if (wr != NULL)
        wr->keep_alive = keep_alive;
    
}

bool web_response_get_keep_alive(struct web_response_t* wr) {
    
    return wr != NULL ? wr->keep_alive : false;
    
}

size_t web_response_write(web_response_p wr, const char* protocol, void* data, size_t data_size) {
    
    if (wr == NULL || protocol == NULL || wr->status_message == NULL)
        return 0;
    
    size_t write_pos = 0;

    size_t status_message_length = strlen(wr->status_message);
    size_t protocol_length = strlen(protocol);
    
    if (data != NULL && write_pos + status_message_length + protocol_length + 7 <= data_size)
        sprintf((char*)data, "%s %d %s\r\n", protocol, wr->status_code, wr->status_message);
    
    write_pos += status_message_length + protocol_length + 7;
    
    size_t headers_length = web_headers_write(wr->headers, NULL, 0);
    
    if (data != NULL && write_pos + headers_length <= data_size)
        web_headers_write(wr->headers, (char*)data + write_pos, data_size - write_pos);
    
    return write_pos + headers_length;
    
}
