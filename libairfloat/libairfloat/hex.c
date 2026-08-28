//
//  hex.c
//  libairfloat
//
//  Created by Kristian Trenskow on 13/08/15.
//
//

#include <stdint.h>

#include "hex.h"

void hex_encode(const void* content, size_t content_size, void* hex, size_t hex_size) {
    
    if (content == NULL || hex == NULL)
        return;
    if (content_size > SIZE_MAX / 2 || hex_size < content_size * 2)
        return;
    
    static const char hex_values[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    const unsigned char* bytes = (const unsigned char*)content;
    char* output = (char*)hex;
    
    for (size_t i = 0 ; i < content_size ; i++) {
        output[i * 2] = hex_values[(bytes[i] >> 4) & 0x0F];
        output[(i * 2) + 1] = hex_values[bytes[i] & 0x0F];
    }
    
}
