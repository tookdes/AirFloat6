//
//  AirFloatiOSAppDelegate.m
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

#import "AppViewController.h"
#import "AirFloatAppDelegate.h"
#import <libairfloat/audiooutput.h>

@interface AirFloatAppDelegate ()

@property (nonatomic, assign) raop_server_p server;

@end
    
@implementation AirFloatAppDelegate {
    UIBackgroundTaskIdentifier *_backgroundTask;
    NSDictionary *_settings;
}

#pragma mark - NSApplication delegates

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    
    self.appViewController = [[[AppViewController alloc] init] autorelease];
    self.window = [[[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds] autorelease];
    
    if ([self.window respondsToSelector:@selector(setRootViewController:)])
        self.window.rootViewController = self.appViewController;
    else {
        self.appViewController.view.frame = CGRectMake(0, 20, 320, 460);
        [self.window addSubview:self.appViewController.view];
    }
    
    [self.window makeKeyAndVisible];
    
    if ([UIApplication instancesRespondToSelector:@selector(registerUserNotificationSettings:)]){
        [application registerUserNotificationSettings:[UIUserNotificationSettings settingsForTypes:UIUserNotificationTypeAlert|UIUserNotificationTypeBadge|UIUserNotificationTypeSound categories:nil]];
    }
    
    /* Load persisted settings before applicationDidBecomeActive starts the
       RAOP server. The previous code read the backing ivar directly and could
       silently start with default/empty values on a cold launch. */
    [self getSettings];
    
    audio_output_session_start();
    
    return YES;
}

- (void)applicationDidBecomeActive:(UIApplication *)application
{
    [self startRaopServer];
}

- (void)applicationWillResignActive:(UIApplication *)application
{
}

- (void)applicationWillEnterForeground:(UIApplication *)application
{
    [self.appViewController handleForegroundTasks];
}

- (void)applicationDidEnterBackground:(UIApplication *)application
{
    [self.appViewController handleBackgroundTasks];
}

- (void)applicationWillTerminate:(UIApplication *)application
{
    if (self.server != NULL) {
        raop_server_destroy(self.server);
        self.server = NULL;
    }
}

#pragma mark - Application Settings

- (NSString *)settingsPath {
    
    NSString* filename = [[[NSBundle mainBundle] bundleIdentifier] stringByAppendingPathExtension:@"plist"];
    NSArray *mypaths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [mypaths objectAtIndex:0];
    NSString *newpath = [documentsDirectory stringByAppendingPathComponent:filename];
    
    return newpath;
}

- (NSDictionary *)getSettings {
    
    if (!_settings) {
        _settings = [[NSDictionary alloc] initWithContentsOfFile:[self settingsPath]];
        if (!_settings)
            _settings = [[NSDictionary alloc] init];
    }
    
    return _settings;
}

- (void)setSettings:(NSDictionary *)settings {
    
    [self willChangeValueForKey:@"settings"];
    
    [_settings release];
    _settings = [settings copy];
    if (!_settings)
        _settings = [[NSDictionary alloc] init];
    
    [_settings writeToFile:[self settingsPath] atomically:YES];
    
    if (self.server) {
        NSString* name = [_settings objectForKey:@"name"];
        NSString* password = [_settings objectForKey:@"password"];
        BOOL authenticationEnabled = [[_settings objectForKey:@"authenticationEnabled"] boolValue];
        
        raop_server_set_settings(self.server, (struct raop_server_settings_t) {
            [name cStringUsingEncoding:NSUTF8StringEncoding],
            (authenticationEnabled && password && [password length] > 0 ? [password cStringUsingEncoding:NSUTF8StringEncoding] : NULL)
        });
    }
    
    [self didChangeValueForKey:@"settings"];
}

#pragma mark - RAOP Server interface

- (void)startRaopServer  {
    
    NSDictionary* settingsDictionary = [self getSettings];
    
    if (!self.server) {
        NSString* name = [settingsDictionary objectForKey:@"name"];
        NSString* password = [settingsDictionary objectForKey:@"password"];
        BOOL authenticationEnabled = [[settingsDictionary objectForKey:@"authenticationEnabled"] boolValue];
        
        struct raop_server_settings_t settings;
        settings.name = [name cStringUsingEncoding:NSUTF8StringEncoding];
        settings.password = (authenticationEnabled && password && [password length] > 0 ? [password cStringUsingEncoding:NSUTF8StringEncoding] : NULL);
        self.server = raop_server_create(settings);
        
        if (!self.server) {
            NSLog(@"Unable to create RAOP server");
            return;
        }
    }
    
    if (!raop_server_is_running(self.server)) {
        uint16_t port = 5000;
        BOOL started = NO;
        while (port < 5010 && !(started = raop_server_start(self.server, port)))
            port++;
        
        if (!started) {
            NSLog(@"Unable to start RAOP server");
            return;
        }
        
        self.appViewController.server = self.server;
    }
}

#pragma mark - Background Notifications

-(void) showNotification:(NSString*)messageTitle
{
    if (!messageTitle) {
        messageTitle = @"Stream started.";
    }
    
    UILocalNotification *notification = [[UILocalNotification alloc] init];
    notification.alertBody = messageTitle;
    notification.fireDate = [NSDate date];
    notification.soundName = UILocalNotificationDefaultSoundName;
    
    [[UIApplication sharedApplication] scheduleLocalNotification:notification];
    [notification release];
}

@end
