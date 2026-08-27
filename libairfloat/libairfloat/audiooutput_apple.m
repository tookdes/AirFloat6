//
//  AudioPlayer.cpp
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

#import <stdint.h>
#import <stdbool.h>

#import <TargetConditionals.h>
#import <AudioToolbox/AudioToolbox.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif
#import <AVFoundation/AVFoundation.h>

#import "log.h"
#import "audiooutput.h"

double hardware_host_time_to_seconds(double host_time);

typedef void (*audio_output_callback)(audio_output_p ao, void* buffer, size_t size, double host_time, void* ctx);

struct audio_output_t {
    AUGraph graph;
    AudioUnit converter_unit;
    AudioUnit mixer_unit;
    bool has_speed_control;
    AudioUnit speed_unit;
    AudioUnit output_unit;
    audio_output_callback callback;
    void* callback_ctx;
};

void audio_output_stop(struct audio_output_t* ao);

static bool _audio_output_check_status(OSStatus status, const char* operation) {
    if (status == noErr)
        return true;
    log_message(LOG_ERROR, "CoreAudio %s failed (%d)", operation, (int)status);
    return false;
}

static bool _audio_output_create_add_unit(struct audio_output_t* ao, OSType type, OSType subtype, OSType manufacturer, AUNode* node, AudioUnit* unit) {
    
    if (ao == NULL || ao->graph == NULL || node == NULL || unit == NULL)
        return false;
    
    AudioComponentDescription desc;
    bzero(&desc, sizeof(AudioComponentDescription));
    desc.componentType = type;
    desc.componentSubType = subtype;
    desc.componentManufacturer = manufacturer;
    
    if (!_audio_output_check_status(AUGraphAddNode(ao->graph, &desc, node), "AUGraphAddNode"))
        return false;
    if (!_audio_output_check_status(AUGraphNodeInfo(ao->graph, *node, NULL, unit), "AUGraphNodeInfo"))
        return false;
    
    UInt32 maximumSlicesPerFrame = 4096;
    OSStatus status = AudioUnitSetProperty(*unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximumSlicesPerFrame, sizeof(UInt32));
    if (status != noErr)
        log_message(LOG_ERROR, "Unable to set maximum AudioUnit slice size (%d)", (int)status);
    
    return true;
}

static bool _audio_output_connect_unit(struct audio_output_t* ao, AUNode output_node, UInt32 output_node_bus, AUNode input_node, UInt32 input_node_bus) {
    
    if (ao == NULL || ao->graph == NULL)
        return false;
    
    AudioUnit output_unit = NULL;
    AudioUnit input_unit = NULL;
    if (!_audio_output_check_status(AUGraphNodeInfo(ao->graph, output_node, NULL, &output_unit), "AUGraphNodeInfo(output)"))
        return false;
    if (!_audio_output_check_status(AUGraphNodeInfo(ao->graph, input_node, NULL, &input_unit), "AUGraphNodeInfo(input)"))
        return false;
    
    AudioStreamBasicDescription out_desc;
    UInt32 size = sizeof(AudioStreamBasicDescription);
    if (!_audio_output_check_status(AudioUnitGetProperty(output_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, output_node_bus, &out_desc, &size), "AudioUnitGetProperty(output format)"))
        return false;
    
    OSStatus direct_status = AudioUnitSetProperty(input_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, input_node_bus, &out_desc, sizeof(AudioStreamBasicDescription));
    if (direct_status == noErr)
        return _audio_output_check_status(AUGraphConnectNodeInput(ao->graph, output_node, output_node_bus, input_node, input_node_bus), "AUGraphConnectNodeInput");
    
    AudioStreamBasicDescription in_desc;
    size = sizeof(AudioStreamBasicDescription);
    if (!_audio_output_check_status(AudioUnitGetProperty(input_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, input_node_bus, &in_desc, &size), "AudioUnitGetProperty(input format)"))
        return false;
    
    AUNode converter_node;
    AudioUnit converter_unit = NULL;
    if (!_audio_output_create_add_unit(ao, kAudioUnitType_FormatConverter, kAudioUnitSubType_AUConverter, kAudioUnitManufacturer_Apple, &converter_node, &converter_unit))
        return false;
    
    if (!_audio_output_check_status(AudioUnitSetProperty(converter_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &out_desc, sizeof(AudioStreamBasicDescription)), "AudioUnitSetProperty(converter input)"))
        return false;
    if (!_audio_output_check_status(AudioUnitSetProperty(converter_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &in_desc, sizeof(AudioStreamBasicDescription)), "AudioUnitSetProperty(converter output)"))
        return false;
    if (!_audio_output_check_status(AUGraphConnectNodeInput(ao->graph, output_node, output_node_bus, converter_node, 0), "AUGraphConnectNodeInput(converter input)"))
        return false;
    if (!_audio_output_check_status(AUGraphConnectNodeInput(ao->graph, converter_node, 0, input_node, input_node_bus), "AUGraphConnectNodeInput(converter output)"))
        return false;
    
    return true;
}

OSStatus _audio_unit_render_callback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
    
    struct audio_output_t* ao = (struct audio_output_t*)inRefCon;
    if (ao == NULL || ioData == NULL || ioData->mNumberBuffers == 0 || ioData->mBuffers[0].mData == NULL)
        return noErr;
    
    bzero(ioData->mBuffers[0].mData, ioData->mBuffers[0].mDataByteSize);
    
    /* Do not assert or log from the real-time render callback. A transient
       AudioUnit state error must not terminate the process or block audio. */
    if (ao->mixer_unit != NULL)
        AudioUnitSetParameter(ao->mixer_unit, kMultiChannelMixerParam_Enable, kAudioUnitScope_Output, 0, 1.0, 0);
    
    audio_output_callback callback = ao->callback;
    void* callback_ctx = ao->callback_ctx;
    if (callback != NULL && inTimeStamp != NULL)
        callback(ao, ioData->mBuffers[0].mData, ioData->mBuffers[0].mDataByteSize, hardware_host_time_to_seconds(inTimeStamp->mHostTime), callback_ctx);
    
    return noErr;
}

static void _audio_output_dispose(struct audio_output_t* ao) {
    if (ao == NULL)
        return;
    
    ao->callback = NULL;
    ao->callback_ctx = NULL;
    
    if (ao->graph != NULL) {
        AUGraphStop(ao->graph);
        AUGraphUninitialize(ao->graph);
        OSStatus status = DisposeAUGraph(ao->graph);
        if (status != noErr)
            log_message(LOG_ERROR, "CoreAudio DisposeAUGraph failed (%d)", (int)status);
        ao->graph = NULL;
    }
}

struct audio_output_t* audio_output_create(struct decoder_output_format_t decoder_output_format) {
    
    if (decoder_output_format.sample_rate == 0 || decoder_output_format.channels == 0 ||
        decoder_output_format.bit_depth == 0 || decoder_output_format.frame_size == 0)
        return NULL;
    
    struct audio_output_t* ao = (struct audio_output_t*)malloc(sizeof(struct audio_output_t));
    if (ao == NULL)
        return NULL;
    bzero(ao, sizeof(struct audio_output_t));
    
    if (!_audio_output_check_status(NewAUGraph(&ao->graph), "NewAUGraph") || ao->graph == NULL)
        goto failed;
    if (!_audio_output_check_status(AUGraphOpen(ao->graph), "AUGraphOpen"))
        goto failed;
    
    AudioStreamBasicDescription in_desc;
    bzero(&in_desc, sizeof(AudioStreamBasicDescription));
    in_desc.mFormatID = kAudioFormatLinearPCM;
    in_desc.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    in_desc.mSampleRate = decoder_output_format.sample_rate;
    in_desc.mChannelsPerFrame = decoder_output_format.channels;
    in_desc.mBitsPerChannel = decoder_output_format.bit_depth;
    in_desc.mFramesPerPacket = 1;
    
    UInt32 size = sizeof(AudioStreamBasicDescription);
    if (!_audio_output_check_status(AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &size, &in_desc), "AudioFormatGetProperty"))
        goto failed;
    
    AUNode mixer_node;
    if (!_audio_output_create_add_unit(ao, kAudioUnitType_Mixer, kAudioUnitSubType_MultiChannelMixer, kAudioUnitManufacturer_Apple, &mixer_node, &ao->mixer_unit))
        goto failed;
    
#if TARGET_OS_MAC
    if (!_audio_output_check_status(AudioUnitSetParameter(ao->mixer_unit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Output, 0, 1.0, 0), "AudioUnitSetParameter(mixer volume)"))
        goto failed;
#endif
    
    AUNode input_node = mixer_node;
    
    OSStatus err = AudioUnitSetProperty(ao->mixer_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &in_desc, sizeof(AudioStreamBasicDescription));
    if (err != noErr) {
        AUNode converter_node;
        if (!_audio_output_create_add_unit(ao, kAudioUnitType_FormatConverter, kAudioUnitSubType_AUConverter, kAudioUnitManufacturer_Apple, &converter_node, &ao->converter_unit))
            goto failed;
        
        if (!_audio_output_check_status(AudioUnitSetProperty(ao->converter_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &in_desc, sizeof(AudioStreamBasicDescription)), "AudioUnitSetProperty(input converter input)"))
            goto failed;
        
        AudioStreamBasicDescription mixer_output_desc;
        size = sizeof(AudioStreamBasicDescription);
        if (!_audio_output_check_status(AudioUnitGetProperty(ao->mixer_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &mixer_output_desc, &size), "AudioUnitGetProperty(mixer input format)"))
            goto failed;
        if (!_audio_output_check_status(AudioUnitSetProperty(ao->converter_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &mixer_output_desc, size), "AudioUnitSetProperty(input converter output)"))
            goto failed;
        if (!_audio_output_connect_unit(ao, converter_node, 0, mixer_node, 0))
            goto failed;
        
        input_node = converter_node;
    }
    
    AURenderCallbackStruct render_callback;
    render_callback.inputProc = _audio_unit_render_callback;
    render_callback.inputProcRefCon = ao;
    if (!_audio_output_check_status(AUGraphSetNodeInputCallback(ao->graph, input_node, 0, &render_callback), "AUGraphSetNodeInputCallback"))
        goto failed;
    
    AUNode output_node;
#if TARGET_OS_IPHONE
    if (!_audio_output_create_add_unit(ao, kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, &output_node, &ao->output_unit))
        goto failed;
#else
    if (!_audio_output_create_add_unit(ao, kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, &output_node, &ao->output_unit))
        goto failed;
#endif
    
    bool use_speed = true;
#if TARGET_OS_IPHONE
    @autoreleasepool {
        use_speed = [[UIDevice currentDevice].systemVersion floatValue] >= 6;
    }
#endif
    
    if (use_speed) {
        AUNode speed_node;
        if (!_audio_output_create_add_unit(ao, kAudioUnitType_FormatConverter, kAudioUnitSubType_Varispeed, kAudioUnitManufacturer_Apple, &speed_node, &ao->speed_unit))
            goto failed;
        if (!_audio_output_connect_unit(ao, mixer_node, 0, speed_node, 0))
            goto failed;
        if (!_audio_output_connect_unit(ao, speed_node, 0, output_node, 0))
            goto failed;
        ao->has_speed_control = true;
    } else {
        if (!_audio_output_connect_unit(ao, mixer_node, 0, output_node, 0))
            goto failed;
    }
    
    if (!_audio_output_check_status(AUGraphInitialize(ao->graph), "AUGraphInitialize"))
        goto failed;
    if (!_audio_output_check_status(AUGraphUpdate(ao->graph, NULL), "AUGraphUpdate"))
        goto failed;
    
    return ao;
    
failed:
    _audio_output_dispose(ao);
    free(ao);
    return NULL;
}

void audio_output_destroy(struct audio_output_t* ao) {
    
    if (ao == NULL)
        return;
    
    _audio_output_dispose(ao);
    free(ao);
}

void audio_output_set_callback(struct audio_output_t* ao, audio_output_callback callback, void* ctx) {
    
    if (ao == NULL)
        return;
    ao->callback = callback;
    ao->callback_ctx = ctx;
}

void audio_output_session_start () {
    
#if TARGET_OS_IPHONE
    @autoreleasepool {
        double sampleRate = 44100.0;
        float frameCount = 4096.0f;
        double bufferLength = (frameCount / sampleRate);
        
        NSError *rateError = nil;
        [[AVAudioSession sharedInstance] setPreferredSampleRate:sampleRate error:&rateError];
        if (rateError)
            log_message(LOG_ERROR, "Error setting SampleRate: %@", [rateError description]);
        
        NSError *bufferError = nil;
        [[AVAudioSession sharedInstance] setPreferredIOBufferDuration:bufferLength error:&bufferError];
        if (bufferError)
            log_message(LOG_ERROR, "Error setting BufferDuration: %@", [bufferError description]);
        
        NSError *categoryError = nil;
        [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:&categoryError];
        if (categoryError)
            log_message(LOG_ERROR, "Error setting Category: %@", [categoryError description]);
        
        NSError *activationError = nil;
        BOOL didActivate = [[AVAudioSession sharedInstance] setActive:YES error:&activationError];
        if (!didActivate) {
            if (activationError)
                log_message(LOG_ERROR, "Could not activate AVAudioSession, Error %@", [activationError localizedDescription]);
            else
                log_message(LOG_ERROR, "Could not activate AVAudioSession");
        }
    }
#endif
}

void audio_output_session_stop () {
    
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSError *deactivationError = nil;
        BOOL didDeactivate = [[AVAudioSession sharedInstance] setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&deactivationError];
        if (!didDeactivate) {
            if (deactivationError)
                log_message(LOG_ERROR, "Could not deactivate AVAudioSession, Error %@", [deactivationError localizedDescription]);
            else
                log_message(LOG_ERROR, "Could not deactivate AVAudioSession");
        }
    }
#endif
}

void audio_output_start(struct audio_output_t* ao) {
    
    if (ao == NULL || ao->graph == NULL)
        return;
    
#if TARGET_OS_IPHONE
    audio_output_session_start();
#endif
    
    OSStatus status = AUGraphStart(ao->graph);
    if (status != noErr) {
        log_message(LOG_ERROR, "CoreAudio AUGraphStart failed (%d)", (int)status);
#if TARGET_OS_IPHONE
        audio_output_session_stop();
#endif
    }
}

void audio_output_stop(struct audio_output_t* ao) {
    
    if (ao == NULL || ao->graph == NULL)
        return;
    
    OSStatus status = AUGraphStop(ao->graph);
    if (status != noErr)
        log_message(LOG_ERROR, "CoreAudio AUGraphStop failed (%d)", (int)status);
    
#if TARGET_OS_IPHONE
    audio_output_session_stop();
#endif
}

void audio_output_flush(struct audio_output_t* ao) {
    
    if (ao == NULL || ao->mixer_unit == NULL)
        return;
    OSStatus status = AudioUnitSetParameter(ao->mixer_unit, kMultiChannelMixerParam_Enable, kAudioUnitScope_Output, 0, 0.0, 0);
    if (status != noErr)
        log_message(LOG_ERROR, "CoreAudio flush failed (%d)", (int)status);
}

double audio_output_get_playback_rate(audio_output_p ao) {
    
    if (ao == NULL || !ao->has_speed_control || ao->speed_unit == NULL)
        return 1.0;
    
    AudioUnitParameterValue value = 1.0;
    OSStatus status = AudioUnitGetParameter(ao->speed_unit, kVarispeedParam_PlaybackRate, kAudioUnitScope_Global, 0, &value);
    if (status != noErr) {
        log_message(LOG_ERROR, "Unable to read playback rate (%d)", (int)status);
        return 1.0;
    }
    
    return value;
}

void audio_output_set_playback_rate(audio_output_p ao, double playback_rate) {
    
    if (ao == NULL || !ao->has_speed_control || ao->speed_unit == NULL)
        return;
    
    AudioUnitParameterValue value = (AudioUnitParameterValue)playback_rate;
    OSStatus status = AudioUnitSetParameter(ao->speed_unit, kVarispeedParam_PlaybackRate, kAudioUnitScope_Global, 0, value, 0);
    if (status != noErr)
        log_message(LOG_ERROR, "Unable to set playback rate (%d)", (int)status);
}

void audio_output_set_volume(struct audio_output_t* ao, double volume) {
    
    if (ao == NULL || ao->mixer_unit == NULL)
        return;
    
    if (volume < 0.0)
        volume = 0.0;
    else if (volume > 1.0)
        volume = 1.0;
    
    OSStatus status = AudioUnitSetParameter(ao->mixer_unit, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, (AudioUnitParameterValue)volume, 0);
    if (status != noErr)
        log_message(LOG_ERROR, "Unable to set output volume (%d)", (int)status);
}

void audio_output_set_muted(struct audio_output_t* ao, bool muted) {
    
    if (ao == NULL || ao->mixer_unit == NULL)
        return;
    OSStatus status = AudioUnitSetParameter(ao->mixer_unit, kMultiChannelMixerParam_Enable, kAudioUnitScope_Input, 0, (muted ? 0.0 : 1.0), 0);
    if (status != noErr)
        log_message(LOG_ERROR, "Unable to change mute state (%d)", (int)status);
}

#endif
