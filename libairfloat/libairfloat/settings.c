//
//  settings_mac.c
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

#include "settings.h"

struct settings_t {
    char* name;
    char* password;
};

struct settings_t* settings_create(const char* name, const char* password) {
    
    struct settings_t* s = (struct settings_t*)malloc(sizeof(struct settings_t));
    if (s == NULL)
        return NULL;
    bzero(s, sizeof(struct settings_t));
    
    settings_set_name(s, name);
    if (s->name == NULL) {
        free(s);
        return NULL;
    }
    
    settings_set_password(s, password);
    if (password != NULL && s->password == NULL) {
        free(s->name);
        free(s);
        return NULL;
    }
    
    return s;
    
}

void settings_destroy(struct settings_t* s) {
    
    if (s == NULL)
        return;
    
    free(s->name);
    free(s->password);
    free(s);
    
}

const char* settings_get_name(struct settings_t* s) {
    
    return s != NULL ? s->name : NULL;
    
}

void settings_set_name(struct settings_t* s, const char* new_name) {
    
    if (s == NULL)
        return;
    
    const char* s_name = new_name;
    if (s_name == NULL || s_name[0] == '\0')
        s_name = "AirFloat";
    
    char* replacement = (char*)malloc(strlen(s_name) + 1);
    if (replacement == NULL)
        return;
    strcpy(replacement, s_name);
    
    free(s->name);
    s->name = replacement;
    
}

const char* settings_get_password(struct settings_t* s) {
    
    return s != NULL ? s->password : NULL;
    
}

void settings_set_password(struct settings_t* s, const char* new_password) {
    
    if (s == NULL)
        return;
    
    char* replacement = NULL;
    if (new_password != NULL) {
        replacement = (char*)malloc(strlen(new_password) + 1);
        if (replacement == NULL)
            return;
        strcpy(replacement, new_password);
    }
    
    free(s->password);
    s->password = replacement;
    
}
