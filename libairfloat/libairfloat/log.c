//
//  log.c
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

#include <stdarg.h>
#include <stdio.h>

#if defined(LOG_SERVER_FILE)
#include <pthread.h>
static pthread_mutex_t write_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#include "log.h"

void log_message(int level, const char* message, ...) {
    
#if defined(LOG_SERVER) || defined(LOG_SERVER_FILE)
    if ((level != LOG_INFO && level != LOG_ERROR) || message == NULL)
        return;
    
    va_list args;
    va_start(args, message);
    
#if defined(LOG_SERVER)
    vprintf(message, args);
    fputc('\n', stdout);
#else
#if defined(LOG_SERVER_FILE)
    pthread_mutex_lock(&write_mutex);
    FILE* log_file = fopen("/var/log/com.tren.AirFloat.log", "a");
    if (log_file != NULL) {
        vfprintf(log_file, message, args);
        fputc('\n', log_file);
        fclose(log_file);
    }
    pthread_mutex_unlock(&write_mutex);
#endif
#endif
    
    va_end(args);
#else
    (void)level;
    (void)message;
#endif
}

void log_data(int level, const void* data, size_t data_size) {
    
#if defined(LOG_SERVER)
    if ((level != LOG_INFO && level != LOG_ERROR) || data == NULL || data_size == 0)
        return;
    
    fwrite(data, 1, data_size, stdout);
    fputc('\n', stdout);
#else
    (void)level;
    (void)data;
    (void)data_size;
#endif
}
