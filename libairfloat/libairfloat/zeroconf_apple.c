//
//  zeroconf_apple.c
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

#if defined(__APPLE__)

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>

#include "log.h"
#include "hardware.h"
#include "settings.h"
#include "mutex.h"
#include "condition.h"
#include "thread.h"

#include "zeroconf.h"

#define ZEROCONF_REGISTRATION_TIMEOUT_MS 5000

static bool _zeroconf_cfstring_to_cstring(CFStringRef string, char* buffer, size_t buffer_size) {
    if (string == NULL || buffer == NULL || buffer_size == 0)
        return false;
    buffer[0] = '\0';
    return CFStringGetCString(string, buffer, (CFIndex)buffer_size, kCFStringEncodingUTF8);
}

struct zeroconf_raop_ad_t {
    CFNetServiceRef service;
    CFRunLoopRef run_loop;
    thread_p thread;
    mutex_p mutex;
    condition_p condition;
    bool registration_complete;
    bool registration_succeeded;
    uint16_t port;
};

void zeroconf_raop_ad_destroy(struct zeroconf_raop_ad_t* za);

void _zeroconf_raop_ad_callback(CFNetServiceRef theService, CFStreamError* error, void* info) {
    struct zeroconf_raop_ad_t* za = (struct zeroconf_raop_ad_t*)info;
    if (za == NULL)
        return;
    
    bool succeeded = (error == NULL || error->error == 0);
    
    mutex_lock(za->mutex);
    za->registration_complete = true;
    za->registration_succeeded = succeeded;
    condition_signal(za->condition);
    mutex_unlock(za->mutex);
    
    if (succeeded)
        log_message(LOG_INFO, "Zeroconf advertising started on port %d", za->port);
    else
        log_message(LOG_ERROR, "Could not start Zeroconf advertisement (domain %d / error %d)", (int)error->domain, (int)error->error);
}

void _zeroconf_raop_ad_run_loop_thread(void* ctx) {
    thread_set_name("RAOP Zeroconf advertising run loop");
    
    struct zeroconf_raop_ad_t* za = (struct zeroconf_raop_ad_t*)ctx;
    if (za == NULL)
        return;
    
    CFRunLoopRef run_loop = CFRunLoopGetCurrent();
    
    mutex_lock(za->mutex);
    za->run_loop = run_loop;
    mutex_unlock(za->mutex);
    
    CFNetServiceClientContext context = { 0, za, NULL, NULL, NULL };
    Boolean client_set = CFNetServiceSetClient(za->service, _zeroconf_raop_ad_callback, &context);
    
    CFStreamError registration_error = { 0, 0 };
    Boolean registration_started = false;
    if (client_set) {
        CFNetServiceScheduleWithRunLoop(za->service, run_loop, kCFRunLoopCommonModes);
        registration_started = CFNetServiceRegisterWithOptions(za->service, kCFNetServiceFlagNoAutoRename, &registration_error);
    }
    
    if (!registration_started) {
        mutex_lock(za->mutex);
        za->registration_complete = true;
        za->registration_succeeded = false;
        condition_signal(za->condition);
        mutex_unlock(za->mutex);
        
        if (!client_set)
            log_message(LOG_ERROR, "Unable to install Zeroconf registration callback");
        else
            log_message(LOG_ERROR, "Unable to start Zeroconf registration (domain %d / error %d)", (int)registration_error.domain, (int)registration_error.error);
    } else
        CFRunLoopRun();
    
    if (client_set) {
        CFNetServiceUnscheduleFromRunLoop(za->service, run_loop, kCFRunLoopCommonModes);
        CFNetServiceSetClient(za->service, NULL, NULL);
        CFNetServiceCancel(za->service);
    }
    
    mutex_lock(za->mutex);
    za->run_loop = NULL;
    mutex_unlock(za->mutex);
}

struct zeroconf_raop_ad_t* zeroconf_raop_ad_create(uint16_t port, const char *name) {
    struct zeroconf_raop_ad_t* za = (struct zeroconf_raop_ad_t*)malloc(sizeof(struct zeroconf_raop_ad_t));
    if (za == NULL)
        return NULL;
    bzero(za, sizeof(struct zeroconf_raop_ad_t));
    
    const char* service_name_c = (name != NULL && name[0] != '\0') ? name : "AirFloat";
    CFStringRef service_name = CFStringCreateWithCString(kCFAllocatorDefault, service_name_c, kCFStringEncodingUTF8);
    if (service_name == NULL) {
        free(za);
        return NULL;
    }
    
    uint64_t hardware_id = hardware_identifier();
    uint8_t* hardware_chars = (uint8_t*)&hardware_id;
    
    CFStringRef hardware_identifier = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%02X%02X%02X%02X%02X%02X"), hardware_chars[2], hardware_chars[3], hardware_chars[4], hardware_chars[5], hardware_chars[6], hardware_chars[7]);
    CFStringRef combined_name = NULL;
    if (hardware_identifier != NULL)
        combined_name = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%@@%@"), hardware_identifier, service_name);
    
    if (hardware_identifier == NULL || combined_name == NULL) {
        if (combined_name != NULL)
            CFRelease(combined_name);
        if (hardware_identifier != NULL)
            CFRelease(hardware_identifier);
        CFRelease(service_name);
        free(za);
        return NULL;
    }
    
    za->service = CFNetServiceCreate(kCFAllocatorDefault, CFSTR(""), CFSTR("_raop._tcp"), combined_name, port);
    za->port = port;
    
    CFRelease(combined_name);
    CFRelease(hardware_identifier);
    CFRelease(service_name);
    
    if (za->service == NULL) {
        free(za);
        return NULL;
    }
    
    CFStringRef keys[16] = { CFSTR("txtvers"), CFSTR("et"), CFSTR("ek"), CFSTR("ss"), CFSTR("sr"), CFSTR("tp"), CFSTR("cn"), CFSTR("da"), CFSTR("sf"), CFSTR("vn"), CFSTR("md"), CFSTR("vs"), CFSTR("sv"), CFSTR("sm"), CFSTR("ch"), CFSTR("sr") };
    CFStringRef values[16] = { CFSTR("1"), CFSTR("0,1"), CFSTR("1"), CFSTR("16"), CFSTR("44100"), CFSTR("TCP,UDP"), CFSTR("1"), CFSTR("true"), CFSTR("0x4"), CFSTR("65537"), CFSTR("0,1,2"), CFSTR("104.29"), CFSTR("false"), CFSTR("false"), CFSTR("2"), CFSTR("44100") };
    
    CFDictionaryRef txt_dictionary = CFDictionaryCreate(kCFAllocatorDefault, (const void**)&keys, (const void**)&values, 16, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef txt_data = NULL;
    if (txt_dictionary != NULL)
        txt_data = CFNetServiceCreateTXTDataWithDictionary(kCFAllocatorDefault, txt_dictionary);
    
    bool txt_configured = (txt_data != NULL && CFNetServiceSetTXTData(za->service, txt_data));
    
    if (txt_data != NULL)
        CFRelease(txt_data);
    if (txt_dictionary != NULL)
        CFRelease(txt_dictionary);
    
    if (!txt_configured) {
        CFRelease(za->service);
        free(za);
        return NULL;
    }
    
    za->mutex = mutex_create();
    za->condition = condition_create();
    if (za->mutex == NULL || za->condition == NULL) {
        if (za->condition != NULL)
            condition_destroy(za->condition);
        if (za->mutex != NULL)
            mutex_destroy(za->mutex);
        CFRelease(za->service);
        free(za);
        return NULL;
    }
    
    mutex_lock(za->mutex);
    za->thread = thread_create_a(_zeroconf_raop_ad_run_loop_thread, za);
    if (za->thread == NULL) {
        mutex_unlock(za->mutex);
        condition_destroy(za->condition);
        mutex_destroy(za->mutex);
        CFRelease(za->service);
        free(za);
        return NULL;
    }
    
    bool timed_out = false;
    while (!za->registration_complete) {
        if (condition_times_wait(za->condition, za->mutex, ZEROCONF_REGISTRATION_TIMEOUT_MS)) {
            timed_out = true;
            break;
        }
    }
    bool registration_succeeded = za->registration_complete && za->registration_succeeded;
    mutex_unlock(za->mutex);
    
    if (!registration_succeeded) {
        if (timed_out)
            log_message(LOG_ERROR, "Timed out waiting for Zeroconf registration");
        zeroconf_raop_ad_destroy(za);
        return NULL;
    }
    
    log_message(LOG_INFO, "Zeroconf configured");
    return za;
}

void zeroconf_raop_ad_destroy(struct zeroconf_raop_ad_t* za) {
    if (za == NULL)
        return;
    
    mutex_lock(za->mutex);
    CFRunLoopRef run_loop = za->run_loop;
    mutex_unlock(za->mutex);
    
    if (run_loop != NULL)
        CFRunLoopStop(run_loop);
    
    thread_destroy(za->thread);
    za->thread = NULL;
    
    condition_destroy(za->condition);
    mutex_destroy(za->mutex);
    
    if (za->service != NULL)
        CFRelease(za->service);
    
    free(za);
}

struct zeroconf_dacp_discover_t {
    CFNetServiceBrowserRef domain_browser;
    CFNetServiceBrowserRef* service_browsers;
    uint32_t service_browsers_count;
    CFNetServiceRef* resolving_services;
    uint32_t resolving_services_count;
    CFRunLoopRef run_loop;
    thread_p thread;
    mutex_p mutex;
    condition_p condition;
    bool run_loop_ready;
    bool destroying;
    zeroconf_dacp_discover_service_found_callback service_found_callback;
    void* service_found_callback_ctx;
};

static bool _zeroconf_dacp_discover_track_service(struct zeroconf_dacp_discover_t* zd, CFNetServiceRef service) {
    if (zd == NULL || service == NULL)
        return false;
    
    mutex_lock(zd->mutex);
    
    if (zd->destroying) {
        mutex_unlock(zd->mutex);
        return false;
    }
    
    for (uint32_t i = 0 ; i < zd->resolving_services_count ; i++) {
        if (zd->resolving_services[i] == service) {
            mutex_unlock(zd->mutex);
            return false;
        }
    }
    
    CFNetServiceRef* services = (CFNetServiceRef*)realloc(zd->resolving_services, sizeof(CFNetServiceRef) * (zd->resolving_services_count + 1));
    if (services == NULL) {
        mutex_unlock(zd->mutex);
        return false;
    }
    
    zd->resolving_services = services;
    CFRetain(service);
    zd->resolving_services[zd->resolving_services_count++] = service;
    
    mutex_unlock(zd->mutex);
    return true;
}

static bool _zeroconf_dacp_discover_untrack_service(struct zeroconf_dacp_discover_t* zd, CFNetServiceRef service, CFRunLoopRef* run_loop) {
    if (run_loop != NULL)
        *run_loop = NULL;
    if (zd == NULL || service == NULL)
        return false;
    
    bool found = false;
    
    mutex_lock(zd->mutex);
    for (uint32_t i = 0 ; i < zd->resolving_services_count ; i++) {
        if (zd->resolving_services[i] == service) {
            for (uint32_t x = i + 1 ; x < zd->resolving_services_count ; x++)
                zd->resolving_services[x - 1] = zd->resolving_services[x];
            zd->resolving_services_count--;
            found = true;
            break;
        }
    }
    
    if (zd->resolving_services_count == 0) {
        free(zd->resolving_services);
        zd->resolving_services = NULL;
    }
    
    if (run_loop != NULL)
        *run_loop = zd->run_loop;
    mutex_unlock(zd->mutex);
    
    return found;
}

static void _zeroconf_dacp_discover_finish_service(struct zeroconf_dacp_discover_t* zd, CFNetServiceRef service) {
    CFRunLoopRef run_loop = NULL;
    if (!_zeroconf_dacp_discover_untrack_service(zd, service, &run_loop))
        return;
    
    CFNetServiceSetClient(service, NULL, NULL);
    CFNetServiceCancel(service);
    if (run_loop != NULL)
        CFNetServiceUnscheduleFromRunLoop(service, run_loop, kCFRunLoopCommonModes);
    CFRelease(service);
}

void _zeroconf_dacp_discover_resolve_callback(CFNetServiceRef service, CFStreamError* error, void* info) {
    struct zeroconf_dacp_discover_t* zd = (struct zeroconf_dacp_discover_t*)info;
    if (zd == NULL || service == NULL)
        return;
    
    bool resolve_ok = (error == NULL || error->error == 0);
    
    char service_name[256];
    bool has_name = _zeroconf_cfstring_to_cstring(CFNetServiceGetName(service), service_name, sizeof(service_name));
    
    struct sockaddr** end_points = NULL;
    uint32_t valid_count = 0;
    
    if (resolve_ok) {
        CFArrayRef addresses = CFNetServiceGetAddressing(service);
        if (addresses != NULL) {
            CFIndex cf_addresses_count = CFArrayGetCount(addresses);
            if (cf_addresses_count > 0 && cf_addresses_count <= UINT32_MAX) {
                uint32_t addresses_count = (uint32_t)cf_addresses_count;
                end_points = (struct sockaddr**)calloc(addresses_count, sizeof(struct sockaddr*));
                if (end_points != NULL) {
                    for (uint32_t i = 0 ; i < addresses_count ; i++) {
                        CFDataRef sockaddr_data = (CFDataRef)CFArrayGetValueAtIndex(addresses, i);
                        if (sockaddr_data == NULL)
                            continue;
                        CFIndex sockaddr_length = CFDataGetLength(sockaddr_data);
                        if (sockaddr_length < (CFIndex)sizeof(struct sockaddr))
                            continue;
                        const struct sockaddr* end_point = (const struct sockaddr*)CFDataGetBytePtr(sockaddr_data);
                        if (end_point == NULL || end_point->sa_len == 0 || (CFIndex)end_point->sa_len > sockaddr_length)
                            continue;
                        struct sockaddr* copy = sockaddr_copy((struct sockaddr*)end_point);
                        if (copy != NULL)
                            end_points[valid_count++] = copy;
                    }
                }
            }
        }
    }
    
    /* Remove the service from the run loop before user code is called. This
       guarantees that a callback destroying zd cannot leave another resolver
       callback queued with a stale info pointer. */
    _zeroconf_dacp_discover_finish_service(zd, service);
    
    zeroconf_dacp_discover_service_found_callback callback = NULL;
    void* callback_ctx = NULL;
    bool destroying = false;
    mutex_lock(zd->mutex);
    destroying = zd->destroying;
    callback = zd->service_found_callback;
    callback_ctx = zd->service_found_callback_ctx;
    mutex_unlock(zd->mutex);
    
    if (!destroying && resolve_ok && valid_count > 0 && has_name && callback != NULL)
        callback(zd, service_name, end_points, valid_count, callback_ctx);
    
    for (uint32_t i = 0 ; i < valid_count ; i++)
        sockaddr_destroy(end_points[i]);
    free(end_points);
    
    if (resolve_ok && has_name)
        log_message(LOG_INFO, "Found DACP Service: %s", service_name);
}

void _zeroconf_dacp_discover_browse_callback(CFNetServiceBrowserRef browser, CFOptionFlags flags, CFTypeRef domainOrService, CFStreamError* error, void* info) {
    struct zeroconf_dacp_discover_t* zd = (struct zeroconf_dacp_discover_t*)info;
    if (zd == NULL || browser == NULL || domainOrService == NULL || (error != NULL && error->error != 0))
        return;
    
    mutex_lock(zd->mutex);
    bool destroying = zd->destroying;
    mutex_unlock(zd->mutex);
    if (destroying || (flags & kCFNetServiceFlagRemove) != 0)
        return;
    
    if (browser == zd->domain_browser) {
        if (CFGetTypeID(domainOrService) != CFStringGetTypeID())
            return;
        
        CFNetServiceClientContext context = { 0, zd, NULL, NULL, NULL };
        CFNetServiceBrowserRef service_browser = CFNetServiceBrowserCreate(kCFAllocatorDefault, _zeroconf_dacp_discover_browse_callback, &context);
        if (service_browser == NULL)
            return;
        
        mutex_lock(zd->mutex);
        if (zd->destroying) {
            mutex_unlock(zd->mutex);
            CFRelease(service_browser);
            return;
        }
        CFNetServiceBrowserRef* browsers = (CFNetServiceBrowserRef*)realloc(zd->service_browsers, sizeof(CFNetServiceBrowserRef) * (zd->service_browsers_count + 1));
        if (browsers == NULL) {
            mutex_unlock(zd->mutex);
            CFRelease(service_browser);
            return;
        }
        zd->service_browsers = browsers;
        zd->service_browsers[zd->service_browsers_count++] = service_browser;
        CFRunLoopRef run_loop = zd->run_loop;
        mutex_unlock(zd->mutex);
        
        if (run_loop != NULL) {
            CFNetServiceBrowserScheduleWithRunLoop(service_browser, run_loop, kCFRunLoopCommonModes);
            CFNetServiceBrowserSearchForServices(service_browser, (CFStringRef)domainOrService, CFSTR("_dacp._tcp."), NULL);
        }
        
        char domain_name[256];
        if (_zeroconf_cfstring_to_cstring((CFStringRef)domainOrService, domain_name, sizeof(domain_name)))
            log_message(LOG_INFO, "Domain found: %s", domain_name);
        
    } else if (CFGetTypeID(domainOrService) == CFNetServiceGetTypeID()) {
        CFNetServiceRef service = (CFNetServiceRef)domainOrService;
        CFArrayRef addresses = CFNetServiceGetAddressing(service);
        
        if (addresses == NULL) {
            if (!_zeroconf_dacp_discover_track_service(zd, service))
                return;
            
            CFNetServiceClientContext context = { 0, zd, NULL, NULL, NULL };
            if (!CFNetServiceSetClient(service, _zeroconf_dacp_discover_resolve_callback, &context)) {
                _zeroconf_dacp_discover_finish_service(zd, service);
                return;
            }
            
            mutex_lock(zd->mutex);
            CFRunLoopRef run_loop = zd->run_loop;
            mutex_unlock(zd->mutex);
            
            if (run_loop == NULL) {
                _zeroconf_dacp_discover_finish_service(zd, service);
                return;
            }
            
            CFNetServiceScheduleWithRunLoop(service, run_loop, kCFRunLoopCommonModes);
            if (!CFNetServiceResolveWithTimeout(service, 30.0, NULL))
                _zeroconf_dacp_discover_finish_service(zd, service);
        } else {
            /* Already-resolved services are owned by the browser callback and
               do not need to enter the tracked asynchronous resolver list. */
            char service_name[256];
            bool has_name = _zeroconf_cfstring_to_cstring(CFNetServiceGetName(service), service_name, sizeof(service_name));
            CFIndex cf_addresses_count = CFArrayGetCount(addresses);
            if (cf_addresses_count > 0 && cf_addresses_count <= UINT32_MAX) {
                uint32_t addresses_count = (uint32_t)cf_addresses_count;
                struct sockaddr** end_points = (struct sockaddr**)calloc(addresses_count, sizeof(struct sockaddr*));
                if (end_points != NULL) {
                    uint32_t valid_count = 0;
                    for (uint32_t i = 0 ; i < addresses_count ; i++) {
                        CFDataRef sockaddr_data = (CFDataRef)CFArrayGetValueAtIndex(addresses, i);
                        if (sockaddr_data == NULL)
                            continue;
                        CFIndex sockaddr_length = CFDataGetLength(sockaddr_data);
                        if (sockaddr_length < (CFIndex)sizeof(struct sockaddr))
                            continue;
                        const struct sockaddr* end_point = (const struct sockaddr*)CFDataGetBytePtr(sockaddr_data);
                        if (end_point == NULL || end_point->sa_len == 0 || (CFIndex)end_point->sa_len > sockaddr_length)
                            continue;
                        struct sockaddr* copy = sockaddr_copy((struct sockaddr*)end_point);
                        if (copy != NULL)
                            end_points[valid_count++] = copy;
                    }
                    
                    mutex_lock(zd->mutex);
                    destroying = zd->destroying;
                    zeroconf_dacp_discover_service_found_callback callback = zd->service_found_callback;
                    void* callback_ctx = zd->service_found_callback_ctx;
                    mutex_unlock(zd->mutex);
                    
                    if (!destroying && has_name && valid_count > 0 && callback != NULL)
                        callback(zd, service_name, end_points, valid_count, callback_ctx);
                    
                    for (uint32_t i = 0 ; i < valid_count ; i++)
                        sockaddr_destroy(end_points[i]);
                    free(end_points);
                }
            }
        }
    }
}

void _zeroconf_dacp_discover_run_loop_ready(CFRunLoopTimerRef timer, void *info) {
    struct zeroconf_dacp_discover_t* zd = (struct zeroconf_dacp_discover_t*)info;
    if (zd == NULL)
        return;
    
    mutex_lock(zd->mutex);
    zd->run_loop_ready = true;
    condition_signal(zd->condition);
    mutex_unlock(zd->mutex);
}

void _zeroconf_dacp_discover_run_loop_thread(void* ctx) {
    thread_set_name("DACP Zeroconf discover run loop");
    
    struct zeroconf_dacp_discover_t* zd = (struct zeroconf_dacp_discover_t*)ctx;
    if (zd == NULL)
        return;
    
    CFRunLoopRef run_loop = CFRunLoopGetCurrent();
    
    mutex_lock(zd->mutex);
    zd->run_loop = run_loop;
    mutex_unlock(zd->mutex);
    
    CFNetServiceBrowserScheduleWithRunLoop(zd->domain_browser, run_loop, kCFRunLoopCommonModes);
    CFNetServiceBrowserSearchForDomains(zd->domain_browser, FALSE, NULL);
    
    CFRunLoopTimerContext timer_context = { 0, zd, NULL, NULL, NULL };
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + .02, 0, 0, 0, _zeroconf_dacp_discover_run_loop_ready, &timer_context);
    if (timer != NULL) {
        CFRunLoopAddTimer(run_loop, timer, kCFRunLoopCommonModes);
        CFRelease(timer);
    } else {
        mutex_lock(zd->mutex);
        zd->run_loop_ready = true;
        condition_signal(zd->condition);
        mutex_unlock(zd->mutex);
    }
    
    CFRunLoopRun();
    
    CFNetServiceBrowserUnscheduleFromRunLoop(zd->domain_browser, run_loop, kCFRunLoopCommonModes);
    CFNetServiceBrowserInvalidate(zd->domain_browser);
    
    mutex_lock(zd->mutex);
    CFNetServiceBrowserRef* service_browsers = zd->service_browsers;
    uint32_t service_browsers_count = zd->service_browsers_count;
    zd->service_browsers = NULL;
    zd->service_browsers_count = 0;
    
    CFNetServiceRef* resolving_services = zd->resolving_services;
    uint32_t resolving_services_count = zd->resolving_services_count;
    zd->resolving_services = NULL;
    zd->resolving_services_count = 0;
    zd->run_loop = NULL;
    mutex_unlock(zd->mutex);
    
    for (uint32_t i = 0 ; i < service_browsers_count ; i++) {
        if (service_browsers[i] != NULL) {
            CFNetServiceBrowserUnscheduleFromRunLoop(service_browsers[i], run_loop, kCFRunLoopCommonModes);
            CFNetServiceBrowserInvalidate(service_browsers[i]);
            CFRelease(service_browsers[i]);
        }
    }
    free(service_browsers);
    
    for (uint32_t i = 0 ; i < resolving_services_count ; i++) {
        if (resolving_services[i] != NULL) {
            CFNetServiceSetClient(resolving_services[i], NULL, NULL);
            CFNetServiceCancel(resolving_services[i]);
            CFNetServiceUnscheduleFromRunLoop(resolving_services[i], run_loop, kCFRunLoopCommonModes);
            CFRelease(resolving_services[i]);
        }
    }
    free(resolving_services);
}

struct zeroconf_dacp_discover_t* zeroconf_dacp_discover_create() {
    struct zeroconf_dacp_discover_t* zd = (struct zeroconf_dacp_discover_t*)malloc(sizeof(struct zeroconf_dacp_discover_t));
    if (zd == NULL)
        return NULL;
    bzero(zd, sizeof(struct zeroconf_dacp_discover_t));
    
    CFNetServiceClientContext context = { 0, zd, NULL, NULL, NULL };
    zd->domain_browser = CFNetServiceBrowserCreate(kCFAllocatorDefault, _zeroconf_dacp_discover_browse_callback, &context);
    zd->mutex = mutex_create();
    zd->condition = condition_create();
    
    if (zd->domain_browser == NULL || zd->mutex == NULL || zd->condition == NULL) {
        if (zd->condition != NULL)
            condition_destroy(zd->condition);
        if (zd->mutex != NULL)
            mutex_destroy(zd->mutex);
        if (zd->domain_browser != NULL)
            CFRelease(zd->domain_browser);
        free(zd);
        return NULL;
    }
    
    /* Browsing is started by set_callback(). This guarantees that a service
       already present on the LAN cannot be discovered before the owner has
       installed its callback. */
    return zd;
}

void zeroconf_dacp_discover_destroy(struct zeroconf_dacp_discover_t* zd) {
    if (zd == NULL)
        return;
    
    mutex_lock(zd->mutex);
    if (zd->destroying) {
        mutex_unlock(zd->mutex);
        return;
    }
    zd->destroying = true;
    zd->service_found_callback = NULL;
    zd->service_found_callback_ctx = NULL;
    CFRunLoopRef run_loop = zd->run_loop;
    thread_p thread = zd->thread;
    mutex_unlock(zd->mutex);
    
    if (run_loop != NULL)
        CFRunLoopStop(run_loop);
    
    if (thread != NULL)
        thread_destroy(thread);
    
    condition_destroy(zd->condition);
    mutex_destroy(zd->mutex);
    
    if (zd->domain_browser != NULL)
        CFRelease(zd->domain_browser);
    
    free(zd);
}

void zeroconf_dacp_discover_set_callback(struct zeroconf_dacp_discover_t* zd, zeroconf_dacp_discover_service_found_callback callback, void* ctx) {
    if (zd == NULL)
        return;
    
    mutex_lock(zd->mutex);
    if (zd->destroying) {
        mutex_unlock(zd->mutex);
        return;
    }
    
    zd->service_found_callback = callback;
    zd->service_found_callback_ctx = ctx;
    
    if (callback != NULL && zd->thread == NULL) {
        zd->run_loop_ready = false;
        zd->thread = thread_create_a(_zeroconf_dacp_discover_run_loop_thread, zd);
        if (zd->thread != NULL) {
            while (!zd->run_loop_ready)
                condition_wait(zd->condition, zd->mutex);
        }
    }
    
    mutex_unlock(zd->mutex);
}

#endif
