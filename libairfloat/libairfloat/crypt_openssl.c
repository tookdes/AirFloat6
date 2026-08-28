//
//  crypt.c
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
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/aes.h>
#include <openssl/md5.h>

#include "crypt_key.h"

struct crypt_aes_t {
    AES_KEY key;
    unsigned char iv[16];
};

static RSA* _private_key = NULL;
static pthread_once_t _private_key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t _private_key_mutex = PTHREAD_MUTEX_INITIALIZER;

static void _crypt_initialize_apple_private_key(void) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == NULL)
        return;
    
    int key_length = (int)strlen(AIRPORT_PRIVATE_KEY_PEM);
    if (key_length > 0 && BIO_write(bio, AIRPORT_PRIVATE_KEY_PEM, key_length) == key_length)
        _private_key = PEM_read_bio_RSAPrivateKey(bio, NULL, NULL, NULL);
    
    BIO_free(bio);
}

static RSA* _crypt_apple_private_key(void) {
    pthread_once(&_private_key_once, _crypt_initialize_apple_private_key);
    return _private_key;
}

size_t crypt_apple_private_encrypt(void* data, size_t data_size, void* encrypted_data, size_t encrypted_data_size) {
    
    if (data == NULL || encrypted_data == NULL || data_size > INT_MAX)
        return 0;
    
    RSA* key = _crypt_apple_private_key();
    if (key == NULL)
        return 0;
    
    int key_size = RSA_size(key);
    if (key_size <= 0 || data_size != (size_t)key_size || encrypted_data_size < (size_t)key_size)
        return 0;
    
    pthread_mutex_lock(&_private_key_mutex);
    int result = RSA_private_encrypt((int)data_size, data, encrypted_data, key, RSA_NO_PADDING);
    pthread_mutex_unlock(&_private_key_mutex);
    
    return result > 0 ? (size_t)result : 0;
}

size_t crypt_apple_private_decrypt(void* encrypted_data, size_t encrypted_data_size, void* data, size_t data_size) {
    
    if (encrypted_data == NULL || data == NULL || encrypted_data_size > INT_MAX)
        return 0;
    
    RSA* key = _crypt_apple_private_key();
    if (key == NULL)
        return 0;
    
    int key_size = RSA_size(key);
    if (key_size <= 0 || encrypted_data_size != (size_t)key_size || data_size < (size_t)key_size)
        return 0;
    
    pthread_mutex_lock(&_private_key_mutex);
    int result = RSA_private_decrypt((int)encrypted_data_size, encrypted_data, data, key, RSA_PKCS1_OAEP_PADDING);
    pthread_mutex_unlock(&_private_key_mutex);
    
    return result > 0 ? (size_t)result : 0;
}

struct crypt_aes_t* crypt_aes_create(void* key, void* iv, size_t size) {
    
    if (key == NULL || iv == NULL || size != 16)
        return NULL;
    
    struct crypt_aes_t* d = (struct crypt_aes_t*)malloc(sizeof(struct crypt_aes_t));
    if (d == NULL)
        return NULL;
    bzero(d, sizeof(struct crypt_aes_t));
    
    if (AES_set_decrypt_key((const unsigned char*)key, 128, &d->key) != 0) {
        free(d);
        return NULL;
    }
    memcpy(d->iv, iv, sizeof(d->iv));
    
    return d;
}

void crypt_aes_destroy(struct crypt_aes_t* d) {
    
    if (d == NULL)
        return;
    bzero(d, sizeof(struct crypt_aes_t));
    free(d);
}

size_t crypt_aes_decrypt(struct crypt_aes_t* d, void* encrypted_data, size_t encrypted_data_size, void* data, size_t data_size) {
    
    if (d == NULL || encrypted_data == NULL || data == NULL || data_size < encrypted_data_size)
        return 0;
    
    unsigned char iv[16];
    memcpy(iv, d->iv, sizeof(iv));
    
    size_t encrypted_len = encrypted_data_size & ~(size_t)0xf;
    memcpy(data, encrypted_data, encrypted_data_size);
    
    if (encrypted_len > 0)
        AES_cbc_encrypt((const unsigned char*)encrypted_data, (unsigned char*)data, encrypted_len, &d->key, iv, AES_DECRYPT);
    
    return encrypted_data_size;
}

void crypt_md5_hash(const void* content, size_t content_size, void* md5, size_t md5_size) {
    
    if (md5 == NULL || md5_size < 16 || (content == NULL && content_size > 0))
        return;
    
    MD5_CTX context;
    MD5_Init(&context);
    if (content_size > 0)
        MD5_Update(&context, content, content_size);
    MD5_Final((unsigned char*)md5, &context);
}
