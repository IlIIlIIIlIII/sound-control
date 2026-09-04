#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <vector>
#include <unistd.h>

namespace {

template <typename T>
bool getProperty(AudioObjectID object,
                 AudioObjectPropertySelector selector,
                 AudioObjectPropertyScope scope,
                 T& value) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr;
}

NSString *stringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value = nullptr;
    if (!getProperty(object, selector, kAudioObjectPropertyScopeGlobal, value) || !value) {
        return @"";
    }
    return CFBridgingRelease(value);
}

}  // namespace

int main() {
    @autoreleasepool {
        AudioDeviceID defaultOutput = kAudioObjectUnknown;
        AudioDeviceID defaultSystem = kAudioObjectUnknown;
        getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                    kAudioObjectPropertyScopeGlobal, defaultOutput);
        getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultSystemOutputDevice,
                    kAudioObjectPropertyScopeGlobal, defaultSystem);

        AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr,
                                           &size) != noErr) return 1;
        std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                       &size, devices.data()) != noErr) return 1;

        NSMutableArray *results = [NSMutableArray array];
        for (AudioDeviceID device : devices) {
            NSString *name = stringProperty(device, kAudioObjectPropertyName);
            if ([name rangeOfString:@"SMSL" options:NSCaseInsensitiveSearch].location == NSNotFound) {
                continue;
            }
            UInt32 alive = 0;
            UInt32 hidden = 0;
            UInt32 canDefault = 0;
            UInt32 canSystem = 0;
            UInt32 running = 0;
            pid_t hogPID = -1;
            getProperty(device, kAudioDevicePropertyDeviceIsAlive,
                        kAudioObjectPropertyScopeGlobal, alive);
            getProperty(device, kAudioDevicePropertyIsHidden,
                        kAudioObjectPropertyScopeGlobal, hidden);
            getProperty(device, kAudioDevicePropertyDeviceCanBeDefaultDevice,
                        kAudioDevicePropertyScopeOutput, canDefault);
            getProperty(device, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice,
                        kAudioDevicePropertyScopeOutput, canSystem);
            getProperty(device, kAudioDevicePropertyDeviceIsRunning,
                        kAudioObjectPropertyScopeGlobal, running);
            getProperty(device, kAudioDevicePropertyHogMode,
                        kAudioObjectPropertyScopeGlobal, hogPID);
            [results addObject:@{
                @"id": @(device),
                @"name": name,
                @"uid": stringProperty(device, kAudioDevicePropertyDeviceUID),
                @"alive": @(alive),
                @"hidden": @(hidden),
                @"canBeDefault": @(canDefault),
                @"canBeSystemDefault": @(canSystem),
                @"running": @(running),
                @"hogPID": @(hogPID),
                @"isDefaultOutput": @(device == defaultOutput),
                @"isDefaultSystemOutput": @(device == defaultSystem),
            }];
        }
        NSData *json = [NSJSONSerialization dataWithJSONObject:results
                                                       options:NSJSONWritingPrettyPrinted
                                                         error:nil];
        if (json) (void)write(STDOUT_FILENO, json.bytes, json.length);
        (void)write(STDOUT_FILENO, "\n", 1);
    }
    return 0;
}
