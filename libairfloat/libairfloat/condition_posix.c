//
//  condition_posix.c
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

#if (__APPLE__)

#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <stdint.h>

#include "condition.h"

pthread_mutex_t* mutex_pthread(struct mutex_t* m);

struct condition_t {
    pthread_cond_t cond;
};

struct condition_t* condition_create() {
    
    struct condition_t* c = (struct condition_t*)malloc(sizeof(struct condition_t));
    if (c == NULL)
        return NULL;
    
    if (pthread_cond_init(&c->cond, NULL) != 0) {
        free(c);
        return NULL;
    }
    
    return c;
}

void condition_destroy(struct condition_t* c) {
    
    if (c == NULL)
        return;
    pthread_cond_destroy(&c->cond);
    free(c);
}

void condition_wait(struct condition_t* c, mutex_p mutex) {
    
    pthread_mutex_t* pthread_mutex = mutex_pthread(mutex);
    if (c != NULL && pthread_mutex != NULL)
        pthread_cond_wait(&c->cond, pthread_mutex);
}

bool condition_times_wait(struct condition_t* c, mutex_p mutex, int milliseconds) {
    
    pthread_mutex_t* pthread_mutex = mutex_pthread(mutex);
    if (c == NULL || pthread_mutex == NULL || milliseconds < 0)
        return true;
    
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return true;
    
    int64_t now = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    int64_t micro = (int64_t)milliseconds * 1000;
    int64_t expected_time = now + micro;
    
    struct timespec req = {
        (time_t)(expected_time / 1000000),
        (long)((expected_time % 1000000) * 1000)
    };
    
    int result = pthread_cond_timedwait(&c->cond, pthread_mutex, &req);
    return (result == ETIMEDOUT || result != 0);
}

void condition_signal(struct condition_t* c) {
    
    if (c != NULL)
        pthread_cond_signal(&c->cond);
}

void condition_broadcast(struct condition_t* c) {
    
    if (c != NULL)
        pthread_cond_broadcast(&c->cond);
}

#endif
