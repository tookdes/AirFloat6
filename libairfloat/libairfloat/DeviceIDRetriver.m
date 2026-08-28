// -----------------------------------
//  DeviceIDRetriver.m (ARC-managed)
// -----------------------------------

#import "DeviceIDRetriver.h"
#import <UIKit/UIKit.h>

uint64_t iOSDeviceID() {
    
    uint64_t returned = 0;
    
    NSUUID *identifier = [[UIDevice currentDevice] identifierForVendor];
    NSString *idString = [identifier UUIDString];
    NSData *idData = [idString dataUsingEncoding:NSUTF8StringEncoding];
    if (idData != nil && [idData length] >= sizeof(returned))
        [idData getBytes:&returned length:sizeof(returned)];
    
    return returned;
}