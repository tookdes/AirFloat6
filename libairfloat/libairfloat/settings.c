//
//  settings.c
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

static char* _settings_copy_string(const char* value) {
    if (value == NULL)
        return NULL;
    
    size_t length = strlen(value);
    char* copy = (char*)malloc(length + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

struct settings_t* settings_create(const char* name, const char* password) {
    
    struct settings_t* settings = (struct settings_t*)malloc(sizeof(struct settings_t));
    if (settings == NULL)
        return NULL;
    settings->name = NULL;
    settings->password = NULL;
    
    const char* effective_name = (name != NULL && name[0] != '\0') ? name : "AirFloat";
    settings->name = _settings_copy_string(effective_name);
    if (settings->name == NULL) {
        free(settings);
        return NULL;
    }
    
    if (password != NULL && password[0] != '\0') {
        settings->password = _settings_copy_string(password);
        if (settings->password == NULL) {
            free(settings->name);
            free(settings);
            return NULL;
        }
    }
    
    return settings;
}

void settings_destroy(struct settings_t* settings) {
    
    if (settings == NULL)
        return;
    
    free(settings->name);
    free(settings->password);
    free(settings);
}

const char* settings_get_name(struct settings_t* settings) {
    return settings != NULL ? settings->name : NULL;
}

void settings_set_name(struct settings_t* settings, const char* name) {
    
    if (settings == NULL)
        return;
    
    const char* effective_name = (name != NULL && name[0] != '\0') ? name : "AirFloat";
    char* replacement = _settings_copy_string(effective_name);
    if (replacement == NULL)
        return;
    
    free(settings->name);
    settings->name = replacement;
}

const char* settings_get_password(struct settings_t* settings) {
    return settings != NULL ? settings->password : NULL;
}

void settings_set_password(struct settings_t* settings, const char* password) {
    
    if (settings == NULL)
        return;
    
    if (password == NULL || password[0] == '\0') {
        free(settings->password);
        settings->password = NULL;
        return;
    }
    
    char* replacement = _settings_copy_string(password);
    if (replacement == NULL)
        return;
    
    free(settings->password);
    settings->password = replacement;
}
