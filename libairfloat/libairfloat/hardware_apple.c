//
//  hardware_apple.c
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

#ifdef __APPLE__

#include <stdint.h>
#include <pthread.h>

#include <mach/mach_time.h>

#include "DeviceIDRetriver.h"

static pthread_once_t _hardware_time_once = PTHREAD_ONCE_INIT;
static uint32_t _hardware_time_to_nanos_numerator = 1;
static uint32_t _hardware_time_to_nanos_denominator = 1;

static void _hardware_initialize_timebase(void) {
    struct mach_timebase_info time_base_info;
    if (mach_timebase_info(&time_base_info) == KERN_SUCCESS &&
        time_base_info.numer != 0 && time_base_info.denom != 0) {
        _hardware_time_to_nanos_numerator = time_base_info.numer;
        _hardware_time_to_nanos_denominator = time_base_info.denom;
    }
}

double hardware_host_time_to_seconds(double host_time) {
    
    pthread_once(&_hardware_time_once, _hardware_initialize_timebase);
    return host_time / (double)_hardware_time_to_nanos_denominator *
           (double)_hardware_time_to_nanos_numerator / 1000000000.0;
    
}

uint64_t hardware_identifier() {
    
    /* iOS 6 exposes identifierForVendor, which is the identity source this
       branch has historically used. The previous en0 scan computed a MAC
       value but discarded it, adding a null-address crash path for no effect. */
    return iOSDeviceID();
    
}

double hardware_get_time() {
    
    return hardware_host_time_to_seconds(mach_absolute_time());
    
}

#endif