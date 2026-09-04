#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <AVFoundation/AVFoundation.h>

#include "AEC.hpp"
#include "DSP.hpp"
#include "DisplayController.h"
#include "MicShared.h"
#include "REWParser.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <mach/mach_time.h>
#include <memory>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static NSString *const MSReloadNotification = @"io.griplabs.macsound.reload";
static NSString *const MSStopNotification = @"io.griplabs.macsound.stop";

static NSString *MSBaseDirectory(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacTools"];
}

static void MSMigrateLegacyDirectory(void) {
    NSFileManager *manager = [NSFileManager defaultManager];
    NSString *legacy = [NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Application Support/MacSound"];
    if (![manager fileExistsAtPath:MSBaseDirectory()] && [manager fileExistsAtPath:legacy]) {
        [manager createDirectoryAtPath:[MSBaseDirectory() stringByDeletingLastPathComponent]
           withIntermediateDirectories:YES attributes:nil error:nil];
        [manager copyItemAtPath:legacy toPath:MSBaseDirectory() error:nil];
        for (NSString *name in @[@"status.json", @"mic-status.json",
                                 @"display-status.json", @"display-recovery.json"]) {
            [manager removeItemAtPath:[MSBaseDirectory() stringByAppendingPathComponent:name]
                                error:nil];
        }
    }
}

static NSString *MSConfigPath(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"config.json"];
}

static NSString *MSFilterPath(BOOL left) {
    return [[MSBaseDirectory() stringByAppendingPathComponent:@"Filters"]
        stringByAppendingPathComponent:left ? @"left.txt" : @"right.txt"];
}

static NSString *MSStatusPath(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"status.json"];
}

static NSString *MSMicStatusPath(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"mic-status.json"];
}

static NSImage *MSMenuBarLaptopImage(void) {
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(18.0, 18.0)];
    [image lockFocus];
    [[NSColor blackColor] setStroke];
    NSBezierPath *screen = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(3.0, 5.0, 12.0, 9.5)
                                                           xRadius:1.2 yRadius:1.2];
    screen.lineWidth = 1.5;
    [screen stroke];
    NSBezierPath *base = [NSBezierPath bezierPath];
    [base moveToPoint:NSMakePoint(1.5, 3.5)];
    [base lineToPoint:NSMakePoint(16.5, 3.5)];
    base.lineWidth = 1.5;
    [base stroke];
    [image unlockFocus];
    [image setTemplate:YES];
    return image;
}

static double MSHostTicksPerSecond(void) {
    static const double value = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return 1.0e9 * static_cast<double>(info.denom) /
            static_cast<double>(info.numer);
    }();
    return value;
}

static void MSWriteMicStatus(NSString *state,
                             NSString *error,
                             NSString *sourceUID,
                             double sampleRate,
                             const MSMicSharedMemory *shared,
                             uint64_t capturedFrames = 0,
                             uint64_t callbackCount = 0,
                             double sourceSampleAdvance = 0,
                             bool echoEnabled = false,
                             macsound::EchoProfile echoProfile = macsound::EchoProfile::quality,
                             const macsound::EchoMetrics& echoMetrics = {},
                             double referenceSampleRate = 0) {
    [[NSFileManager defaultManager] createDirectoryAtPath:MSBaseDirectory()
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSString *echoState = @"off";
    if (echoEnabled) {
        if (echoMetrics.inputClipping) {
            echoState = @"input-clipping";
        } else if (!echoMetrics.active) {
            echoState = @"idle";
        } else if (echoMetrics.doubleTalk) {
            echoState = @"double-talk";
        } else if (echoMetrics.convergence == macsound::EchoConvergenceState::tracking) {
            echoState = @"tracking";
        } else if (echoMetrics.convergence == macsound::EchoConvergenceState::learning) {
            echoState = @"learning";
        } else {
            echoState = @"active";
        }
    }
    NSDictionary *status = @{
        @"state": state ?: @"unknown",
        @"error": error ?: @"",
        @"source": @"M2 Input 1",
        @"sourceUID": sourceUID ?: @"",
        @"virtualDeviceUID": @"io.griplabs.macsound.mic.device",
        @"sampleRate": @(sampleRate),
        @"overruns": @(MSMicOverruns(shared)),
        @"underruns": @(MSMicUnderruns(shared)),
        @"capturedFrames": @(capturedFrames),
        @"callbackCount": @(callbackCount),
        @"sourceSampleAdvance": @(sourceSampleAdvance),
        @"echoCancellationEnabled": @(echoEnabled),
        @"echoCancellationState": echoState,
        @"echoCancellationProfile":
            [NSString stringWithUTF8String:macsound::echoProfileName(echoProfile)],
        @"echoReductionDB": @(echoMetrics.reductionDB),
        @"echoLinearReductionDB": @(echoMetrics.linearReductionDB),
        @"echoResidualSuppressionDB": @(echoMetrics.residualSuppressionDB),
        @"echoConvergenceState":
            [NSString stringWithUTF8String:macsound::echoConvergenceName(
                echoMetrics.convergence)],
        @"echoNonlinearActive": @(echoMetrics.nonlinearActive),
        @"echoInputClipping": @(echoMetrics.inputClipping),
        @"echoPathChangeCount": @(echoMetrics.pathChangeCount),
        @"echoEstimatedDelayMs": @(echoMetrics.estimatedDelayMs),
        @"echoReferenceUnderruns": @(echoMetrics.referenceUnderruns),
        @"echoReferenceSampleRate": @(referenceSampleRate),
        @"updatedAt": @([[NSDate date] timeIntervalSince1970]),
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:status options:0 error:nil];
    [data writeToFile:MSMicStatusPath() options:NSDataWritingAtomic error:nil];
}

static NSMutableDictionary *MSLoadConfig(void) {
    NSData *data = [NSData dataWithContentsOfFile:MSConfigPath()];
    if (data) {
        id object = [NSJSONSerialization JSONObjectWithData:data
                                                    options:NSJSONReadingMutableContainers
                                                      error:nil];
        if ([object isKindOfClass:[NSDictionary class]]) return [object mutableCopy];
    }
    return [@{@"enabled": @NO} mutableCopy];
}

static BOOL MSSaveConfig(NSDictionary *config) {
    [[NSFileManager defaultManager] createDirectoryAtPath:MSBaseDirectory()
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSData *data = [NSJSONSerialization dataWithJSONObject:config
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:nil];
    return data && [data writeToFile:MSConfigPath() options:NSDataWritingAtomic error:nil];
}

static void MSWriteStatus(NSString *state,
                          NSString *error,
                          double sampleRate,
                          double preampDB,
                          uint64_t xruns,
                          float inputPeak,
                          float outputPeak) {
    [[NSFileManager defaultManager] createDirectoryAtPath:MSBaseDirectory()
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSDictionary *status = @{
        @"state": state ?: @"unknown",
        @"error": error ?: @"",
        @"sampleRate": @(sampleRate),
        @"preampDB": @(preampDB),
        @"xruns": @(xruns),
        @"inputPeak": @(inputPeak),
        @"outputPeak": @(outputPeak),
        @"updatedAt": @([[NSDate date] timeIntervalSince1970]),
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:status options:0 error:nil];
    [data writeToFile:MSStatusPath() options:NSDataWritingAtomic error:nil];
}

static BOOL MSGetProperty(AudioObjectID object,
                          AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope,
                          void *data,
                          UInt32 *size) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, size, data) == noErr;
}

static BOOL MSSetProperty(AudioObjectID object,
                          AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope,
                          const void *data,
                          UInt32 size) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectSetPropertyData(object, &address, 0, nullptr, size, data) == noErr;
}

static BOOL MSSetDefaultDevice(AudioObjectPropertySelector selector, AudioDeviceID device) {
    return device != kAudioObjectUnknown &&
        MSSetProperty(kAudioObjectSystemObject, selector, kAudioObjectPropertyScopeGlobal,
                      &device, sizeof(device));
}

static AudioDeviceID MSDefaultDevice(AudioObjectPropertySelector selector) {
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    return MSGetProperty(kAudioObjectSystemObject, selector, kAudioObjectPropertyScopeGlobal,
                         &device, &size)
        ? device : kAudioObjectUnknown;
}

static NSString *MSStringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (!MSGetProperty(object, selector, kAudioObjectPropertyScopeGlobal, &value, &size) || !value) {
        return nil;
    }
    return CFBridgingRelease(value);
}

static UInt32 MSUInt32Property(AudioObjectID object,
                               AudioObjectPropertySelector selector,
                               AudioObjectPropertyScope scope) {
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    MSGetProperty(object, selector, scope, &value, &size);
    return value;
}

static Float64 MSDoubleProperty(AudioObjectID object,
                                AudioObjectPropertySelector selector,
                                AudioObjectPropertyScope scope) {
    Float64 value = 0;
    UInt32 size = sizeof(value);
    MSGetProperty(object, selector, scope, &value, &size);
    return value;
}

static UInt32 MSChannelCount(AudioObjectID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, scope,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    std::vector<std::byte> storage(size);
    auto *list = reinterpret_cast<AudioBufferList *>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) return 0;
    UInt32 channels = 0;
    for (UInt32 index = 0; index < list->mNumberBuffers; ++index) {
        channels += list->mBuffers[index].mNumberChannels;
    }
    return channels;
}

static AudioDeviceID MSFindDevice(NSString *uid) {
    if (uid.length == 0) return kAudioObjectUnknown;
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
        return kAudioObjectUnknown;
    }
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return kAudioObjectUnknown;
    }
    for (AudioObjectID device : devices) {
        if ([MSStringProperty(device, kAudioDevicePropertyDeviceUID) isEqualToString:uid]) return device;
    }
    return kAudioObjectUnknown;
}

static NSArray<NSDictionary<NSString *, NSString *> *> *MSPhysicalOutputDevices(void) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address,
                                       0, nullptr, &size) != noErr || size == 0) {
        return @[];
    }
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                   &size, devices.data()) != noErr) {
        return @[];
    }

    NSMutableArray<NSDictionary<NSString *, NSString *> *> *result = [NSMutableArray array];
    for (AudioObjectID device : devices) {
        if (MSChannelCount(device, kAudioDevicePropertyScopeOutput) < 2) continue;
        const UInt32 transport = MSUInt32Property(device, kAudioDevicePropertyTransportType,
                                                   kAudioObjectPropertyScopeGlobal);
        if (transport == kAudioDeviceTransportTypeVirtual ||
            transport == kAudioDeviceTransportTypeAggregate) {
            continue;
        }
        NSString *uid = MSStringProperty(device, kAudioDevicePropertyDeviceUID);
        NSString *name = MSStringProperty(device, kAudioObjectPropertyName);
        if (uid.length == 0 || name.length == 0) continue;
        [result addObject:@{@"uid": uid, @"name": name}];
    }
    [result sortUsingComparator:^NSComparisonResult(NSDictionary *left, NSDictionary *right) {
        return [left[@"name"] localizedStandardCompare:right[@"name"]];
    }];
    return result;
}

static AudioDeviceID MSFindInputDeviceNamed(NSString *name) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
        return kAudioObjectUnknown;
    }
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return kAudioObjectUnknown;
    }
    for (AudioObjectID device : devices) {
        if (MSChannelCount(device, kAudioDevicePropertyScopeInput) > 0 &&
            [MSStringProperty(device, kAudioObjectPropertyName) isEqualToString:name]) {
            return device;
        }
    }
    return kAudioObjectUnknown;
}

static AudioDeviceID MSFindFallbackInputDevice(NSString *excludedSourceUID) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
        return kAudioObjectUnknown;
    }
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return kAudioObjectUnknown;
    }

    AudioDeviceID firstPhysicalInput = kAudioObjectUnknown;
    for (AudioObjectID device : devices) {
        if (MSChannelCount(device, kAudioDevicePropertyScopeInput) == 0) continue;
        NSString *uid = MSStringProperty(device, kAudioDevicePropertyDeviceUID);
        if ([uid isEqualToString:@"io.griplabs.macsound.mic.device"] ||
            (excludedSourceUID.length > 0 && [uid isEqualToString:excludedSourceUID])) {
            continue;
        }
        const UInt32 transport = MSUInt32Property(device, kAudioDevicePropertyTransportType,
                                                   kAudioObjectPropertyScopeGlobal);
        if (transport == kAudioDeviceTransportTypeBuiltIn) return device;
        if (transport != kAudioDeviceTransportTypeVirtual &&
            firstPhysicalInput == kAudioObjectUnknown) {
            firstPhysicalInput = device;
        }
    }
    return firstPhysicalInput;
}

static void MSSelectFallbackInputIfNeeded(NSString *excludedSourceUID) {
    const AudioDeviceID current = MSDefaultDevice(kAudioHardwarePropertyDefaultInputDevice);
    NSString *currentUID = current == kAudioObjectUnknown
        ? nil : MSStringProperty(current, kAudioDevicePropertyDeviceUID);
    if (current != kAudioObjectUnknown &&
        ![currentUID isEqualToString:@"io.griplabs.macsound.mic.device"]) {
        return;
    }
    const AudioDeviceID fallback = MSFindFallbackInputDevice(excludedSourceUID);
    (void)MSSetDefaultDevice(kAudioHardwarePropertyDefaultInputDevice, fallback);
}

static AudioObjectID MSCurrentProcessObject(void) {
    pid_t pid = getpid();
    AudioObjectID process = kAudioObjectUnknown;
    UInt32 size = sizeof(process);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslatePIDToProcessObject,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                        sizeof(pid), &pid, &size, &process);
    return status == noErr ? process : kAudioObjectUnknown;
}

static NSString *MSOSStatusDescription(OSStatus status) {
    UInt32 value = CFSwapInt32HostToBig(static_cast<UInt32>(status));
    char chars[5] = {};
    memcpy(chars, &value, 4);
    BOOL printable = YES;
    for (int i = 0; i < 4; ++i) {
        if (chars[i] < 32 || chars[i] > 126) printable = NO;
    }
    return printable
        ? [NSString stringWithFormat:@"'%s' (%d)", chars, status]
        : [NSString stringWithFormat:@"%d", status];
}

static UInt32 MSFramesForChannel(const AudioBufferList *list, UInt32 channel) {
    if (!list) return 0;
    UInt32 offset = channel;
    for (UInt32 bufferIndex = 0; bufferIndex < list->mNumberBuffers; ++bufferIndex) {
        const AudioBuffer &buffer = list->mBuffers[bufferIndex];
        if (offset < buffer.mNumberChannels) {
            if (!buffer.mData || buffer.mNumberChannels == 0) return 0;
            return buffer.mDataByteSize /
                static_cast<UInt32>(sizeof(Float32) * buffer.mNumberChannels);
        }
        offset -= buffer.mNumberChannels;
    }
    return 0;
}

static float MSReadChannel(const AudioBufferList *list, UInt32 channel, UInt32 frame) {
    UInt32 offset = channel;
    for (UInt32 bufferIndex = 0; bufferIndex < list->mNumberBuffers; ++bufferIndex) {
        const AudioBuffer &buffer = list->mBuffers[bufferIndex];
        if (offset < buffer.mNumberChannels && buffer.mData) {
            const Float32 *samples = static_cast<const Float32 *>(buffer.mData);
            return samples[frame * buffer.mNumberChannels + offset];
        }
        offset -= buffer.mNumberChannels;
    }
    return 0.0f;
}

static void MSWriteChannel(AudioBufferList *list, UInt32 channel, UInt32 frame, float value) {
    UInt32 offset = channel;
    for (UInt32 bufferIndex = 0; bufferIndex < list->mNumberBuffers; ++bufferIndex) {
        AudioBuffer &buffer = list->mBuffers[bufferIndex];
        if (offset < buffer.mNumberChannels && buffer.mData) {
            Float32 *samples = static_cast<Float32 *>(buffer.mData);
            samples[frame * buffer.mNumberChannels + offset] = value;
            return;
        }
        offset -= buffer.mNumberChannels;
    }
}

static void MSAtomicMaximum(std::atomic<float> &destination, float value) {
    float current = destination.load(std::memory_order_relaxed);
    while (current < value &&
           !destination.compare_exchange_weak(current, value,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
    }
}

@interface MSEngine : NSObject <NSMenuDelegate>
- (void)start;
- (void)shutdown;
- (OSStatus)processInput:(const AudioBufferList *)inputData
                  output:(AudioBufferList *)outputData
              outputTime:(const AudioTimeStamp *)outputTime;
- (OSStatus)captureMicInput:(const AudioBufferList *)inputData
                  timeStamp:(const AudioTimeStamp *)timeStamp
                      output:(AudioBufferList *)outputData;
@end

@implementation MSEngine {
    NSMutableDictionary *_config;
    AudioDeviceID _targetDevice;
    AudioObjectID _tap;
    AudioDeviceID _aggregateDevice;
    AudioDeviceIOProcID _ioProc;
    UInt32 _tapInputChannelOffset;
    UInt32 _maxFrames;
    std::vector<double> _leftScratch;
    std::vector<double> _rightScratch;
    std::vector<float> _referenceRouteLeft;
    std::vector<float> _referenceRouteRight;
    std::unique_ptr<macsound::StereoDSP> _dsp;
    std::atomic<uint64_t> _xruns;
    std::atomic<float> _inputPeak;
    std::atomic<float> _outputPeak;
    double _sampleRate;
    double _preampDB;
    BOOL _running;
    BOOL _eqActive;
    BOOL _waitingForTarget;
    NSTimer *_monitorTimer;
    AudioDeviceIOProcID _micIOProc;
    AudioDeviceID _micDevice;
    std::vector<Float32> _micScratch;
    MSMicSharedMemory *_micShared;
    BOOL _micRunning;
    BOOL _micPermissionResolved;
    BOOL _micPermissionGranted;
    BOOL _micNeedsDefaultSelection;
    NSString *_micSourceUID;
    std::atomic<uint64_t> _micCapturedFrames;
    std::atomic<uint64_t> _micCallbackCount;
    std::atomic<double> _micLastSampleTime;
    double _micReportedSampleTime;
    std::vector<Float32> _micProcessedScratch;
    std::vector<Float32> _micReferenceLeft;
    std::vector<Float32> _micReferenceRight;
    std::unique_ptr<macsound::StereoReferenceTimeline> _referenceTimeline;
    std::unique_ptr<macsound::EchoCanceller> _echoCanceller;
    std::atomic<bool> _echoCancellationEnabled;
    macsound::EchoProfile _echoProfile;
    uint64_t _micReferenceGeneration;
    MTDisplayController *_displayController;
    NSStatusItem *_statusItem;
    NSMenu *_statusMenu;
    NSMenu *_outputDeviceMenu;
    NSMenu *_echoProfileMenu;
    NSMenuItem *_engineStatusItem;
    NSMenuItem *_eqStatusItem;
    NSMenuItem *_echoStatusItem;
    NSMenuItem *_menuErrorItem;
    NSMenuItem *_outputDeviceRootItem;
    NSMenuItem *_eqMenuItem;
    NSMenuItem *_echoMenuItem;
    NSMenuItem *_echoProfileRootItem;
    NSString *_menuError;
}

static OSStatus MSIOProc(AudioDeviceID device,
                         const AudioTimeStamp *now,
                         const AudioBufferList *inputData,
                         const AudioTimeStamp *inputTime,
                         AudioBufferList *outputData,
                         const AudioTimeStamp *outputTime,
                         void *clientData) {
    (void)device;
    (void)now;
    (void)inputTime;
    MSEngine *engine = (__bridge MSEngine *)clientData;
    return [engine processInput:inputData output:outputData outputTime:outputTime];
}

static OSStatus MSMicDeviceIOProc(AudioDeviceID device,
                                  const AudioTimeStamp *now,
                                  const AudioBufferList *inputData,
                                  const AudioTimeStamp *inputTime,
                                  AudioBufferList *outputData,
                                  const AudioTimeStamp *outputTime,
                                  void *clientData) {
    (void)device;
    (void)now;
    (void)outputTime;
    MSEngine *engine = (__bridge MSEngine *)clientData;
    return [engine captureMicInput:inputData timeStamp:inputTime output:outputData];
}

- (NSMenuItem *)menuItemWithTitle:(NSString *)title action:(SEL)action {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    return item;
}

- (void)setupStatusItem {
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    _statusItem.button.image = MSMenuBarLaptopImage();
    _statusItem.button.image.accessibilityDescription = @"MacTools";
    _statusItem.button.toolTip = @"MacTools";

    _statusMenu = [[NSMenu alloc] initWithTitle:@"MacTools"];
    _statusMenu.delegate = self;
    [self buildStatusMenuItems];
    _statusItem.menu = _statusMenu;
    [self refreshStatusMenu];
}

- (void)buildStatusMenuItems {
    _engineStatusItem = [[NSMenuItem alloc] initWithTitle:@"MacTools · 상태 확인 중"
                                                   action:nil keyEquivalent:@""];
    _engineStatusItem.enabled = NO;
    [_statusMenu addItem:_engineStatusItem];
    _eqStatusItem = [[NSMenuItem alloc] initWithTitle:@"EQ: 상태 확인 중"
                                               action:nil keyEquivalent:@""];
    _eqStatusItem.enabled = NO;
    [_statusMenu addItem:_eqStatusItem];
    _echoStatusItem = [[NSMenuItem alloc] initWithTitle:@"스피커 소리 제거: 상태 확인 중"
                                                 action:nil keyEquivalent:@""];
    _echoStatusItem.enabled = NO;
    [_statusMenu addItem:_echoStatusItem];
    _menuErrorItem = [[NSMenuItem alloc] initWithTitle:@""
                                                action:nil keyEquivalent:@""];
    _menuErrorItem.enabled = NO;
    _menuErrorItem.hidden = YES;
    [_statusMenu addItem:_menuErrorItem];
    [_statusMenu addItem:[NSMenuItem separatorItem]];

    _outputDeviceRootItem = [[NSMenuItem alloc] initWithTitle:@"출력 장치"
                                                       action:nil keyEquivalent:@""];
    _outputDeviceMenu = [[NSMenu alloc] initWithTitle:@"출력 장치"];
    _outputDeviceRootItem.submenu = _outputDeviceMenu;
    [_statusMenu addItem:_outputDeviceRootItem];

    _eqMenuItem = [self menuItemWithTitle:@"EQ 활성화" action:@selector(menuToggleEQ:)];
    [_statusMenu addItem:_eqMenuItem];
    _echoMenuItem = [self menuItemWithTitle:@"스피커 소리 제거"
                                      action:@selector(menuToggleEcho:)];
    [_statusMenu addItem:_echoMenuItem];

    _echoProfileRootItem = [[NSMenuItem alloc] initWithTitle:@"제거 강도"
                                                      action:nil keyEquivalent:@""];
    _echoProfileMenu = [[NSMenu alloc] initWithTitle:@"제거 강도"];
    for (NSArray<NSString *> *entry in @[@[@"최고 음질", @"quality"],
                                          @[@"균형", @"balanced"],
                                          @[@"강한 제거", @"strong"]]) {
        NSMenuItem *item = [self menuItemWithTitle:entry[0]
                                            action:@selector(menuSelectEchoProfile:)];
        item.representedObject = entry[1];
        [_echoProfileMenu addItem:item];
    }
    _echoProfileRootItem.submenu = _echoProfileMenu;
    [_statusMenu addItem:_echoProfileRootItem];
    [_statusMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *reinitialize = [self menuItemWithTitle:@"외장 디스플레이 다시 초기화"
                                                 action:@selector(menuReinitializeDisplays:)];
    reinitialize.keyEquivalent = @"r";
    reinitialize.keyEquivalentModifierMask = NSEventModifierFlagControl |
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [_statusMenu addItem:reinitialize];
    NSMenuItem *swap = [self menuItemWithTitle:@"MO32U24 위치 스왑"
                                        action:@selector(menuSwapDisplays:)];
    swap.keyEquivalent = @"s";
    swap.keyEquivalentModifierMask = NSEventModifierFlagControl |
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [_statusMenu addItem:swap];
    [_statusMenu addItem:[NSMenuItem separatorItem]];
    [_statusMenu addItem:[self menuItemWithTitle:@"엔진 다시 로드"
                                          action:@selector(menuReload:)]];
    [_statusMenu addItem:[self menuItemWithTitle:@"설정 열기…"
                                          action:@selector(menuOpenSettings:)]];
}

- (NSDictionary *)statusDictionaryAtPath:(NSString *)path {
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return nil;
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [object isKindOfClass:[NSDictionary class]] ? object : nil;
}

- (NSString *)engineStateTitle:(NSString *)state {
    NSDictionary<NSString *, NSString *> *titles = @{
        @"running": @"실행 중",
        @"bypassed": @"바이패스",
        @"stopped": @"종료됨",
        @"permission required": @"권한 필요",
        @"waiting for target": @"출력 장치 대기",
        @"waiting for filters": @"필터 대기",
        @"target disconnected": @"출력 장치 연결 안 됨",
        @"error": @"오류",
    };
    return titles[state] ?: (state.length > 0 ? state : @"상태 알 수 없음");
}

- (NSString *)echoProfileTitle:(NSString *)profile {
    return [@{@"quality": @"최고 음질", @"balanced": @"균형", @"strong": @"강한 제거"}
        objectForKey:profile] ?: @"최고 음질";
}

- (void)refreshOutputDeviceMenu {
    [_outputDeviceMenu removeAllItems];
    NSString *selectedUID = _config[@"targetDeviceUID"];
    NSString *selectedName = _config[@"targetDeviceName"] ?: @"선택한 장치";
    NSArray<NSDictionary<NSString *, NSString *> *> *devices = MSPhysicalOutputDevices();
    BOOL selectedConnected = NO;
    for (NSDictionary<NSString *, NSString *> *device in devices) {
        NSMenuItem *item = [self menuItemWithTitle:device[@"name"]
                                            action:@selector(menuSelectOutputDevice:)];
        item.representedObject = device;
        item.state = [device[@"uid"] isEqualToString:selectedUID]
            ? NSControlStateValueOn : NSControlStateValueOff;
        if (item.state == NSControlStateValueOn) selectedConnected = YES;
        [_outputDeviceMenu addItem:item];
    }
    if (selectedUID.length > 0 && !selectedConnected) {
        if (_outputDeviceMenu.numberOfItems > 0) {
            [_outputDeviceMenu insertItem:[NSMenuItem separatorItem] atIndex:0];
        }
        NSMenuItem *missing = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%@ (연결 안 됨)", selectedName]
                    action:nil keyEquivalent:@""];
        missing.enabled = NO;
        missing.state = NSControlStateValueOn;
        [_outputDeviceMenu insertItem:missing atIndex:0];
    } else if (_outputDeviceMenu.numberOfItems == 0) {
        NSMenuItem *empty = [[NSMenuItem alloc] initWithTitle:@"사용 가능한 물리 출력 장치 없음"
                                                       action:nil keyEquivalent:@""];
        empty.enabled = NO;
        [_outputDeviceMenu addItem:empty];
    }
}

- (void)refreshStatusMenu {
    _config = MSLoadConfig();
    NSDictionary *status = [self statusDictionaryAtPath:MSStatusPath()];
    NSDictionary *micStatus = [self statusDictionaryAtPath:MSMicStatusPath()];
    NSString *state = status[@"state"];
    NSString *targetName = _config[@"targetDeviceName"] ?: @"선택 안 됨";
    BOOL targetConnected = MSFindDevice(_config[@"targetDeviceUID"]) != kAudioObjectUnknown;
    NSString *connection = targetConnected ? @"" : @" · 연결 안 됨";
    _engineStatusItem.title = [NSString stringWithFormat:@"MacTools · %@ · %@%@",
        [self engineStateTitle:state], targetName, connection];

    BOOL eqEnabled = [_config[@"enabled"] boolValue];
    NSString *left = _config[@"leftSourceName"] ?: @"L 미선택";
    NSString *right = _config[@"rightSourceName"] ?: @"R 미선택";
    _eqStatusItem.title = [NSString stringWithFormat:@"EQ: %@ · %@ · %@",
        eqEnabled ? @"켜짐" : @"꺼짐", left, right];

    BOOL echoEnabled = !_config[@"echoCancellationEnabled"] ||
        [_config[@"echoCancellationEnabled"] boolValue];
    NSString *profile = _config[@"echoCancellationProfile"] ?: @"quality";
    NSString *echoState = micStatus[@"echoCancellationState"];
    NSDictionary<NSString *, NSString *> *echoStates = @{
        @"active": @"동작 중", @"double-talk": @"발화 보호",
        @"learning": @"학습 중", @"tracking": @"경로 재학습",
        @"input-clipping": @"M2 입력 과부하",
        @"idle": @"대기", @"off": @"꺼짐",
    };
    NSString *echoStateTitle = echoEnabled ? (echoStates[echoState] ?: @"준비 중") : @"꺼짐";
    _echoStatusItem.title = echoEnabled
        ? [NSString stringWithFormat:@"스피커 소리 제거: %@ · %@ · %.1f dB",
            echoStateTitle, [self echoProfileTitle:profile],
            [micStatus[@"echoReductionDB"] doubleValue]]
        : @"스피커 소리 제거: 꺼짐";

    NSString *statusError = status[@"error"];
    NSString *error = _menuError.length > 0 ? _menuError : statusError;
    _menuErrorItem.hidden = error.length == 0;
    _menuErrorItem.title = error.length > 0
        ? [NSString stringWithFormat:@"오류: %@", error] : @"";
    _statusItem.button.toolTip = _engineStatusItem.title;

    [self refreshOutputDeviceMenu];
    _eqMenuItem.state = eqEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    _echoMenuItem.state = echoEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    NSString *targetUID = _config[@"targetDeviceUID"];
    _eqMenuItem.enabled = targetUID.length > 0 || eqEnabled;
    _echoMenuItem.enabled = targetUID.length > 0 || echoEnabled;
    _echoProfileRootItem.enabled = echoEnabled;
    for (NSMenuItem *item in _echoProfileMenu.itemArray) {
        item.state = [item.representedObject isEqualToString:profile]
            ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

- (void)menuWillOpen:(NSMenu *)menu {
    if (menu == _statusMenu) {
        [self refreshStatusMenu];
    }
}

- (BOOL)saveMenuConfig:(NSMutableDictionary *)newConfig {
    if (!MSSaveConfig(newConfig)) {
        _menuError = @"설정을 저장하지 못했습니다. 기존 설정을 유지합니다.";
        NSBeep();
        [self refreshStatusMenu];
        return NO;
    }
    _menuError = nil;
    _config = newConfig;
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:MSReloadNotification object:nil
                    userInfo:nil deliverImmediately:YES];
    return YES;
}

- (void)menuSelectOutputDevice:(NSMenuItem *)sender {
    NSDictionary *device = sender.representedObject;
    if (![device isKindOfClass:[NSDictionary class]]) return;
    NSMutableDictionary *config = MSLoadConfig();
    config[@"targetDeviceUID"] = device[@"uid"];
    config[@"targetDeviceName"] = device[@"name"];
    config[@"resumeWhenTargetReturns"] = @NO;
    [self saveMenuConfig:config];
}

- (void)menuToggleEQ:(id)sender {
    (void)sender;
    NSMutableDictionary *config = MSLoadConfig();
    config[@"enabled"] = @(![config[@"enabled"] boolValue]);
    config[@"resumeWhenTargetReturns"] = @NO;
    [self saveMenuConfig:config];
}

- (void)menuToggleEcho:(id)sender {
    (void)sender;
    NSMutableDictionary *config = MSLoadConfig();
    const BOOL enabled = !config[@"echoCancellationEnabled"] ||
        [config[@"echoCancellationEnabled"] boolValue];
    config[@"echoCancellationEnabled"] = @(!enabled);
    if (!config[@"echoCancellationProfile"]) config[@"echoCancellationProfile"] = @"quality";
    [self saveMenuConfig:config];
}

- (void)menuSelectEchoProfile:(NSMenuItem *)sender {
    NSString *profile = sender.representedObject;
    if (![profile isKindOfClass:[NSString class]]) return;
    NSMutableDictionary *config = MSLoadConfig();
    config[@"echoCancellationProfile"] = profile;
    [self saveMenuConfig:config];
}

- (void)menuReinitializeDisplays:(id)sender {
    (void)sender;
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:MTDisplayReinitializeNotification object:nil
                    userInfo:nil deliverImmediately:YES];
}

- (void)menuSwapDisplays:(id)sender {
    (void)sender;
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:MTDisplaySwapNotification object:nil
                    userInfo:nil deliverImmediately:YES];
}

- (void)menuReload:(id)sender {
    (void)sender;
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:MSReloadNotification object:nil
                    userInfo:nil deliverImmediately:YES];
}

- (NSURL *)outerApplicationURL {
    NSURL *url = NSBundle.mainBundle.bundleURL;
    while (url.path.length > 1) {
        if ([url.lastPathComponent isEqualToString:@"MacTools.app"]) return url;
        url = [url URLByDeletingLastPathComponent];
    }
    return nil;
}

- (void)menuOpenSettings:(id)sender {
    (void)sender;
    NSURL *applicationURL = [self outerApplicationURL];
    if (!applicationURL) {
        _menuError = @"MacTools 설정 앱을 찾지 못했습니다.";
        return;
    }
    NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    configuration.createsNewApplicationInstance = NO;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:applicationURL
                                          configuration:configuration
                                      completionHandler:^(NSRunningApplication *application,
                                                          NSError *error) {
        (void)application;
        if (error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_menuError = [NSString stringWithFormat:@"설정 앱 열기 실패: %@",
                                                               error.localizedDescription];
            });
        }
    }];
}

- (void)start {
    MSMigrateLegacyDirectory();
    _referenceTimeline = std::make_unique<macsound::StereoReferenceTimeline>();
    _displayController = [MTDisplayController new];
    [_displayController start];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:self
                                                        selector:@selector(reloadNotification:)
                                                            name:MSReloadNotification
                                                          object:nil];
    [[NSDistributedNotificationCenter defaultCenter] addObserver:self
                                                        selector:@selector(stopNotification:)
                                                            name:MSStopNotification
                                                          object:nil];
    _monitorTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                     target:self
                                                   selector:@selector(monitor:)
                                                   userInfo:nil
                                                    repeats:YES];
    _micShared = MSMicOpenSharedMemory();
    [self reconcile];
    [self setupStatusItem];
    AVAuthorizationStatus permission =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (permission == AVAuthorizationStatusNotDetermined) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                completionHandler:^(BOOL granted) {
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_micPermissionResolved = YES;
                self->_micPermissionGranted = granted;
                if (granted) {
                    [self reconcileMic];
                } else {
                    MSWriteMicStatus(@"permission required",
                                     @"시스템 설정 > 개인정보 보호 및 보안 > 마이크에서 MacTools Engine을 허용하세요.",
                                     self->_config[@"micDeviceUID"], 0, self->_micShared);
                }
            });
        }];
    } else {
        _micPermissionResolved = YES;
        _micPermissionGranted = permission == AVAuthorizationStatusAuthorized;
        if (_micPermissionGranted) {
            [self reconcileMic];
        } else {
            MSWriteMicStatus(@"permission required",
                             @"시스템 설정 > 개인정보 보호 및 보안 > 마이크에서 MacTools Engine을 허용하세요.",
                             _config[@"micDeviceUID"], 0, _micShared);
        }
    }
}

- (void)reloadNotification:(NSNotification *)notification {
    (void)notification;
    [self stopMic];
    [self reconcile];
    if (_micPermissionGranted) [self reconcileMic];
}

- (void)stopNotification:(NSNotification *)notification {
    (void)notification;
    [self shutdown];
    [NSApp terminate:nil];
}

- (void)monitor:(NSTimer *)timer {
    (void)timer;
    [self monitorMic];
    if (_running) {
        if (MSFindDevice(_config[@"targetDeviceUID"]) == kAudioObjectUnknown) {
            _waitingForTarget = YES;
            if ([_config[@"enabled"] boolValue]) {
                _config[@"enabled"] = @NO;
                _config[@"resumeWhenTargetReturns"] = @YES;
                MSSaveConfig(_config);
            }
            [self stopRoute];
            MSWriteStatus(@"target disconnected",
                          @"EQ가 해제되었습니다. USB 장치를 다시 연결하면 자동으로 활성화하고 기본 출력으로 지정합니다.",
                          0, 0, _xruns.load(), 0, 0);
            return;
        }
        const double currentRate = MSDoubleProperty(_targetDevice,
                                                     kAudioDevicePropertyNominalSampleRate,
                                                     kAudioObjectPropertyScopeGlobal);
        if (currentRate > 0 && std::abs(currentRate - _sampleRate) >= 0.5) {
            [self reconcile];
            return;
        }
        MSWriteStatus(@"running", @"", _sampleRate, _preampDB, _xruns.load(),
                      _inputPeak.exchange(0, std::memory_order_relaxed),
                      _outputPeak.exchange(0, std::memory_order_relaxed));
    } else if (_waitingForTarget &&
               ([_config[@"resumeWhenTargetReturns"] boolValue] ||
                !_config[@"echoCancellationEnabled"] ||
                [_config[@"echoCancellationEnabled"] boolValue]) &&
               MSFindDevice(_config[@"targetDeviceUID"]) != kAudioObjectUnknown) {
        _waitingForTarget = NO;
        if ([_config[@"resumeWhenTargetReturns"] boolValue]) {
            _config[@"enabled"] = @YES;
            _config[@"resumeWhenTargetReturns"] = @NO;
            MSSaveConfig(_config);
        }
        [self reconcile];
    }
}

- (void)monitorMic {
    if (!_micPermissionResolved) return;
    if (!_micPermissionGranted) {
        MSWriteMicStatus(@"permission required",
                         @"시스템 설정 > 개인정보 보호 및 보안 > 마이크에서 MacTools Engine을 허용하세요.",
                         _config[@"micDeviceUID"], 0, _micShared);
        return;
    }
    if (_micRunning) {
        if (_micDevice == kAudioObjectUnknown || MSFindDevice(_micSourceUID) == kAudioObjectUnknown) {
            MSSelectFallbackInputIfNeeded(_micSourceUID);
            [self stopMic];
            MSWriteMicStatus(@"source disconnected",
                             @"MacTools Mic을 내리고 M2 연결을 기다리는 중입니다. 연결되면 자동으로 복원합니다.",
                             _micSourceUID, 0, _micShared);
            return;
        }
        if (_micNeedsDefaultSelection) {
            const AudioDeviceID virtualMic = MSFindDevice(@"io.griplabs.macsound.mic.device");
            if (virtualMic != kAudioObjectUnknown) {
                (void)MSSetDefaultDevice(kAudioHardwarePropertyDefaultInputDevice, virtualMic);
                _micNeedsDefaultSelection = NO;
            }
        }
        const uint64_t capturedFrames = _micCapturedFrames.exchange(
            0, std::memory_order_relaxed);
        const uint64_t callbackCount = _micCallbackCount.exchange(
            0, std::memory_order_relaxed);
        const double currentSampleTime = _micLastSampleTime.load(
            std::memory_order_relaxed);
        const double sampleAdvance = _micReportedSampleTime > 0 && currentSampleTime > 0
            ? currentSampleTime - _micReportedSampleTime : 0;
        _micReportedSampleTime = currentSampleTime;
        const bool echoEnabled = _echoCancellationEnabled.load(std::memory_order_relaxed);
        const macsound::EchoMetrics echoMetrics = _echoCanceller
            ? _echoCanceller->metrics(_referenceTimeline ? _referenceTimeline->underruns() : 0)
            : macsound::EchoMetrics{};
        MSWriteMicStatus(@"running", @"", _micSourceUID, 48000.0, _micShared,
                         capturedFrames, callbackCount, sampleAdvance,
                         echoEnabled, _echoProfile, echoMetrics,
                         _referenceTimeline ? _referenceTimeline->sourceSampleRate() : 0.0);
    } else {
        [self reconcileMic];
    }
}

- (void)reconcileMic {
    [self stopMic];
    if (!_config) _config = MSLoadConfig();
    const bool echoEnabled = !_config[@"echoCancellationEnabled"] ||
        [_config[@"echoCancellationEnabled"] boolValue];
    NSString *profileName = _config[@"echoCancellationProfile"] ?: @"quality";
    _echoProfile = macsound::echoProfileFromName(profileName.UTF8String);
    _echoCancellationEnabled.store(echoEnabled, std::memory_order_relaxed);
    if (_config[@"micEnabled"] && ![_config[@"micEnabled"] boolValue]) {
        MSSelectFallbackInputIfNeeded(_config[@"micDeviceUID"]);
        MSWriteMicStatus(@"disabled", @"", _config[@"micDeviceUID"], 0, _micShared);
        return;
    }

    NSString *sourceUID = _config[@"micDeviceUID"];
    AudioDeviceID device = MSFindDevice(sourceUID);
    if (device == kAudioObjectUnknown) {
        device = MSFindInputDeviceNamed(@"M2");
        if (device != kAudioObjectUnknown) {
            sourceUID = MSStringProperty(device, kAudioDevicePropertyDeviceUID);
            if (sourceUID.length > 0) {
                _config[@"micDeviceUID"] = sourceUID;
                _config[@"micEnabled"] = @YES;
                MSSaveConfig(_config);
            }
        }
    }
    if (device == kAudioObjectUnknown || MSChannelCount(device, kAudioDevicePropertyScopeInput) < 1) {
        _micSourceUID = sourceUID;
        MSSelectFallbackInputIfNeeded(sourceUID);
        MSWriteMicStatus(@"source disconnected",
                         @"M2 입력 1을 찾지 못했습니다. MacTools Mic은 숨겨지고 다른 입력이 기본값이 됩니다.",
                         sourceUID, 0, _micShared);
        return;
    }

    const Float64 requiredRate = 48000.0;
    const Float64 currentRate = MSDoubleProperty(device, kAudioDevicePropertyNominalSampleRate,
                                                  kAudioObjectPropertyScopeGlobal);
    if (std::abs(currentRate - requiredRate) >= 0.5 &&
        !MSSetProperty(device, kAudioDevicePropertyNominalSampleRate,
                       kAudioObjectPropertyScopeGlobal, &requiredRate, sizeof(requiredRate))) {
        MSWriteMicStatus(@"error", @"M2를 48 kHz로 설정하지 못했습니다.",
                         sourceUID, currentRate, _micShared);
        return;
    }

    UInt32 maximumFrames = MSUInt32Property(device, kAudioDevicePropertyBufferFrameSize,
                                             kAudioObjectPropertyScopeGlobal);
    maximumFrames = std::max<UInt32>(maximumFrames, 8192u);
    _micScratch.assign(maximumFrames, 0.0f);
    _micProcessedScratch.assign(maximumFrames, 0.0f);
    _micReferenceLeft.assign(maximumFrames, 0.0f);
    _micReferenceRight.assign(maximumFrames, 0.0f);
    if (echoEnabled) {
        _echoCanceller = std::make_unique<macsound::EchoCanceller>();
        _echoCanceller->setProfile(_echoProfile);
        if (!_echoCanceller->valid()) {
            _echoCanceller.reset();
            _echoCancellationEnabled.store(false, std::memory_order_relaxed);
        }
    } else {
        _echoCanceller.reset();
    }

    OSStatus status = AudioDeviceCreateIOProcID(device, MSMicDeviceIOProc,
                                                (__bridge void *)self, &_micIOProc);
    if (status == noErr) {
        _micDevice = device;
        _micSourceUID = sourceUID;
        _micRunning = YES;
        _micNeedsDefaultSelection = YES;
        _micCapturedFrames.store(0, std::memory_order_relaxed);
        _micCallbackCount.store(0, std::memory_order_relaxed);
        _micLastSampleTime.store(0, std::memory_order_relaxed);
        _micReportedSampleTime = 0;
        _micReferenceGeneration = _referenceTimeline ? _referenceTimeline->generation() : 0;
        MSMicResetReader(_micShared);
        MSMicSetEngineOnline(_micShared, true);
        status = AudioDeviceStart(_micDevice, _micIOProc);
    }
    if (status != noErr) {
        NSString *message = [NSString stringWithFormat:
            @"M2 입력 시작 실패: %@. 시스템 설정의 마이크 권한을 확인하세요.",
            MSOSStatusDescription(status)];
        [self stopMic];
        MSWriteMicStatus(@"permission required", message, sourceUID, requiredRate, _micShared);
        return;
    }

    const AudioDeviceID virtualMic = MSFindDevice(@"io.griplabs.macsound.mic.device");
    NSString *message = @"";
    if (virtualMic != kAudioObjectUnknown) {
        (void)MSSetDefaultDevice(kAudioHardwarePropertyDefaultInputDevice, virtualMic);
        _micNeedsDefaultSelection = NO;
    } else {
        message = @"MacTools Mic 장치가 게시되기를 기다리는 중입니다.";
    }
    MSWriteMicStatus(@"running", message, sourceUID, requiredRate, _micShared);
}

- (OSStatus)captureMicInput:(const AudioBufferList *)inputData
                  timeStamp:(const AudioTimeStamp *)timeStamp
                      output:(AudioBufferList *)outputData {
    if (outputData) {
        for (UInt32 index = 0; index < outputData->mNumberBuffers; ++index) {
            AudioBuffer &buffer = outputData->mBuffers[index];
            if (buffer.mData) memset(buffer.mData, 0, buffer.mDataByteSize);
        }
    }
    if (!_micRunning || !_micIOProc || !_micShared || !inputData) {
        return noErr;
    }
    const UInt32 frameCount = MSFramesForChannel(inputData, 0);
    if (frameCount == 0 || frameCount > _micScratch.size()) return noErr;
    for (UInt32 frame = 0; frame < frameCount; ++frame) {
        _micScratch[frame] = MSReadChannel(inputData, 0, frame);
    }
    const Float32 *micOutput = _micScratch.data();
    if (_echoCancellationEnabled.load(std::memory_order_relaxed) &&
        _echoCanceller && _referenceTimeline) {
        const uint64_t generation = _referenceTimeline->generation();
        if (generation != _micReferenceGeneration) {
            _echoCanceller->reset();
            _micReferenceGeneration = generation;
        }
        const uint64_t inputHostTime = timeStamp &&
            (timeStamp->mFlags & kAudioTimeStampHostTimeValid)
            ? timeStamp->mHostTime : mach_absolute_time();
        const bool referenceAvailable = _referenceTimeline->render(
            inputHostTime, MSHostTicksPerSecond(),
            _micReferenceLeft.data(), _micReferenceRight.data(), frameCount);
        _echoCanceller->process(_micScratch.data(), _micReferenceLeft.data(),
                                _micReferenceRight.data(), _micProcessedScratch.data(),
                                frameCount, referenceAvailable);
        micOutput = _micProcessedScratch.data();
    }
    (void)MSMicWrite(_micShared, micOutput, frameCount);
    _micCapturedFrames.fetch_add(frameCount, std::memory_order_relaxed);
    _micCallbackCount.fetch_add(1, std::memory_order_relaxed);
    const Float64 sampleTime = timeStamp &&
        (timeStamp->mFlags & kAudioTimeStampSampleTimeValid)
        ? timeStamp->mSampleTime : 0.0;
    const uint64_t hostTime = timeStamp &&
        (timeStamp->mFlags & kAudioTimeStampHostTimeValid)
        ? timeStamp->mHostTime : mach_absolute_time();
    if (sampleTime > 0) _micLastSampleTime.store(sampleTime, std::memory_order_relaxed);
    MSMicPublishTimestamp(_micShared, sampleTime, hostTime);
    return noErr;
}

- (void)stopMic {
    _micRunning = NO;
    MSMicSetEngineOnline(_micShared, false);
    if (_micDevice != kAudioObjectUnknown && _micIOProc) {
        (void)AudioDeviceStop(_micDevice, _micIOProc);
        (void)AudioDeviceDestroyIOProcID(_micDevice, _micIOProc);
    }
    _micIOProc = nullptr;
    _micDevice = kAudioObjectUnknown;
    _micScratch.clear();
    _micProcessedScratch.clear();
    _micReferenceLeft.clear();
    _micReferenceRight.clear();
    _echoCanceller.reset();
}

- (void)reconcile {
    [self stopRoute];
    _config = MSLoadConfig();
    BOOL migratedEchoSettings = NO;
    if (!_config[@"echoCancellationEnabled"]) {
        _config[@"echoCancellationEnabled"] = @YES;
        migratedEchoSettings = YES;
    }
    if (!_config[@"echoCancellationProfile"]) {
        _config[@"echoCancellationProfile"] = @"quality";
        migratedEchoSettings = YES;
    }
    if (migratedEchoSettings) MSSaveConfig(_config);
    BOOL eqEnabled = [_config[@"enabled"] boolValue];
    const BOOL echoEnabled = !_config[@"echoCancellationEnabled"] ||
        [_config[@"echoCancellationEnabled"] boolValue];
    _echoCancellationEnabled.store(echoEnabled, std::memory_order_relaxed);

    if (!eqEnabled && [_config[@"resumeWhenTargetReturns"] boolValue]) {
        NSString *waitingUID = _config[@"targetDeviceUID"];
        if (MSFindDevice(waitingUID) != kAudioObjectUnknown) {
            _config[@"enabled"] = @YES;
            _config[@"resumeWhenTargetReturns"] = @NO;
            MSSaveConfig(_config);
            eqEnabled = YES;
        }
    }
    if (!eqEnabled && !echoEnabled) {
        _waitingForTarget = NO;
        MSWriteStatus(@"bypassed", @"", 0, 0, _xruns.load(), 0, 0);
        return;
    }

    NSString *targetUID = _config[@"targetDeviceUID"];
    if (targetUID.length == 0) {
        MSWriteStatus(@"waiting for target", @"제어 앱에서 출력 장치를 선택하세요.",
                      0, 0, _xruns.load(), 0, 0);
        return;
    }

    _targetDevice = MSFindDevice(targetUID);
    if (_targetDevice == kAudioObjectUnknown) {
        _waitingForTarget = YES;
        if (eqEnabled) {
            _config[@"enabled"] = @NO;
            _config[@"resumeWhenTargetReturns"] = @YES;
            MSSaveConfig(_config);
        }
        MSWriteStatus(@"target disconnected",
                      @"선택한 스피커 연결을 기다리는 동안 EQ와 반향 제거를 안전하게 우회합니다.",
                      0, 0, _xruns.load(), 0, 0);
        return;
    }
    const UInt32 transport = MSUInt32Property(_targetDevice,
                                               kAudioDevicePropertyTransportType,
                                               kAudioObjectPropertyScopeGlobal);
    const UInt32 targetOutputChannels = MSChannelCount(_targetDevice,
                                                        kAudioDevicePropertyScopeOutput);
    if (transport == kAudioDeviceTransportTypeVirtual ||
        transport == kAudioDeviceTransportTypeAggregate || targetOutputChannels < 2) {
        MSWriteStatus(@"error", @"2채널 이상의 물리 출력 장치만 사용할 수 있습니다.",
                      0, 0, _xruns.load(), 0, 0);
        return;
    }

    const Float64 targetRate = MSDoubleProperty(_targetDevice,
                                                 kAudioDevicePropertyNominalSampleRate,
                                                 kAudioObjectPropertyScopeGlobal);
    if (targetRate <= 0) {
        MSWriteStatus(@"error", @"대상 장치의 샘플레이트를 읽을 수 없습니다.",
                      0, 0, _xruns.load(), 0, 0);
        return;
    }

    std::unique_ptr<macsound::StereoDSP> dsp;
    if (eqEnabled) {
        const auto left = macsound::parseREWConfigurablePEQFile(
            std::filesystem::path(MSFilterPath(YES).fileSystemRepresentation),
            macsound::Channel::left);
        const auto right = macsound::parseREWConfigurablePEQFile(
            std::filesystem::path(MSFilterPath(NO).fileSystemRepresentation),
            macsound::Channel::right);
        if (!left || !right) {
            NSString *message = !left
                ? [NSString stringWithUTF8String:left.error.c_str()]
                : [NSString stringWithUTF8String:right.error.c_str()];
            MSWriteStatus(@"waiting for filters", message, 0, 0, _xruns.load(), 0, 0);
            return;
        }
        dsp = std::make_unique<macsound::StereoDSP>();
        std::string dspError;
        if (!dsp->configure(left.filters, right.filters, targetRate, dspError)) {
            MSWriteStatus(@"error", [NSString stringWithUTF8String:dspError.c_str()],
                          targetRate, 0, _xruns.load(), 0, 0);
            return;
        }
    }

    AudioObjectID processObject = MSCurrentProcessObject();
    if (processObject == kAudioObjectUnknown) {
        MSWriteStatus(@"error", @"엔진의 Core Audio 프로세스 식별자를 얻지 못했습니다.",
                      targetRate, 0, _xruns.load(), 0, 0);
        return;
    }

    CATapDescription *tapDescription = [[CATapDescription alloc]
        initExcludingProcesses:@[@(processObject)] andDeviceUID:targetUID withStream:0];
    tapDescription.name = @"MacTools Output Reference Tap";
    [tapDescription setPrivate:YES];
    tapDescription.muteBehavior = CATapMutedWhenTapped;
    tapDescription.processRestoreEnabled = YES;

    OSStatus status = AudioHardwareCreateProcessTap(tapDescription, &_tap);
    if (status != noErr || _tap == kAudioObjectUnknown) {
        NSString *message = [NSString stringWithFormat:
            @"오디오 탭 생성 실패: %@. 시스템 설정 > 개인정보 보호 및 보안 > 시스템 오디오 녹음에서 MacTools Engine 권한을 확인하세요.",
            MSOSStatusDescription(status)];
        MSWriteStatus(@"permission required", message, targetRate, 0,
                      _xruns.load(), 0, 0);
        return;
    }

    NSString *tapUID = MSStringProperty(_tap, kAudioTapPropertyUID);
    AudioStreamBasicDescription tapFormat{};
    UInt32 formatSize = sizeof(tapFormat);
    if (tapUID.length == 0 ||
        !MSGetProperty(_tap, kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal,
                       &tapFormat, &formatSize) || tapFormat.mChannelsPerFrame < 2) {
        [self stopRoute];
        MSWriteStatus(@"error", @"선택한 출력 스트림의 스테레오 탭을 구성할 수 없습니다.",
                      targetRate, 0, _xruns.load(), 0, 0);
        return;
    }

    NSDictionary *targetDescription = @{
        @kAudioSubDeviceUIDKey: targetUID,
        @kAudioSubDeviceDriftCompensationKey: @NO,
    };
    NSDictionary *subTapDescription = @{
        @kAudioSubTapUIDKey: tapUID,
        @kAudioSubTapDriftCompensationKey: @YES,
        @kAudioSubTapDriftCompensationQualityKey: @(kAudioAggregateDriftCompensationMaxQuality),
    };
    NSString *aggregateUID = [NSString stringWithFormat:@"io.griplabs.macsound.private.%@",
                                                        NSUUID.UUID.UUIDString];
    NSDictionary *aggregateDescription = @{
        @kAudioAggregateDeviceNameKey: @"MacTools Private EQ Route",
        @kAudioAggregateDeviceUIDKey: aggregateUID,
        @kAudioAggregateDeviceMainSubDeviceKey: targetUID,
        @kAudioAggregateDeviceClockDeviceKey: targetUID,
        @kAudioAggregateDeviceIsPrivateKey: @YES,
        @kAudioAggregateDeviceIsStackedKey: @YES,
        @kAudioAggregateDeviceTapAutoStartKey: @NO,
        @kAudioAggregateDeviceSubDeviceListKey: @[targetDescription],
        @kAudioAggregateDeviceTapListKey: @[subTapDescription],
    };
    status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregateDescription,
                                                 &_aggregateDevice);
    if (status != noErr || _aggregateDevice == kAudioObjectUnknown) {
        [self stopRoute];
        MSWriteStatus(@"error",
                      [NSString stringWithFormat:@"비공개 오디오 경로 생성 실패: %@",
                                                 MSOSStatusDescription(status)],
                      targetRate, 0, _xruns.load(), 0, 0);
        return;
    }

    _tapInputChannelOffset = MSChannelCount(_targetDevice, kAudioDevicePropertyScopeInput);
    const UInt32 aggregateInputs = MSChannelCount(_aggregateDevice, kAudioDevicePropertyScopeInput);
    const UInt32 aggregateOutputs = MSChannelCount(_aggregateDevice, kAudioDevicePropertyScopeOutput);
    if (aggregateInputs < _tapInputChannelOffset + 2 || aggregateOutputs < 2) {
        [self stopRoute];
        MSWriteStatus(@"error", @"비공개 오디오 경로의 채널 구성이 올바르지 않습니다.",
                      targetRate, 0, _xruns.load(), 0, 0);
        return;
    }

    AudioValueRange frameRange{};
    UInt32 rangeSize = sizeof(frameRange);
    AudioObjectPropertyAddress rangeAddress{kAudioDevicePropertyBufferFrameSizeRange,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(_aggregateDevice, &rangeAddress, 0, nullptr,
                                   &rangeSize, &frameRange) == noErr &&
        frameRange.mMaximum > 0 && frameRange.mMaximum <= 65536) {
        _maxFrames = static_cast<UInt32>(std::ceil(frameRange.mMaximum));
    } else {
        _maxFrames = std::max<UInt32>(8192, MSUInt32Property(
            _aggregateDevice, kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal));
    }
    _leftScratch.assign(_maxFrames, 0.0);
    _rightScratch.assign(_maxFrames, 0.0);
    _referenceRouteLeft.assign(_maxFrames, 0.0f);
    _referenceRouteRight.assign(_maxFrames, 0.0f);
    _dsp = std::move(dsp);
    _eqActive = eqEnabled;
    _xruns.store(0);
    _inputPeak.store(0);
    _outputPeak.store(0);
    _sampleRate = targetRate;
    _preampDB = _dsp ? _dsp->preampDB() : 0.0;

    status = AudioDeviceCreateIOProcID(_aggregateDevice, MSIOProc,
                                       (__bridge void *)self, &_ioProc);
    if (status == noErr) {
        _running = YES;
        status = AudioDeviceStart(_aggregateDevice, _ioProc);
    }
    if (status != noErr) {
        _running = NO;
        [self stopRoute];
        MSWriteStatus(@"error",
                      [NSString stringWithFormat:@"오디오 엔진 시작 실패: %@",
                                                 MSOSStatusDescription(status)],
                      targetRate, 0, _xruns.load(), 0, 0);
        return;
    }

    _waitingForTarget = NO;
    if ([_config[@"resumeWhenTargetReturns"] boolValue]) {
        _config[@"resumeWhenTargetReturns"] = @NO;
        _config[@"enabled"] = @YES;
        MSSaveConfig(_config);
    }
    const BOOL defaultOutputSet =
        MSSetDefaultDevice(kAudioHardwarePropertyDefaultOutputDevice, _targetDevice);
    const BOOL systemOutputSet =
        MSSetDefaultDevice(kAudioHardwarePropertyDefaultSystemOutputDevice, _targetDevice);
    NSString *routeError = defaultOutputSet && systemOutputSet
        ? @""
        : @"오디오 처리는 실행 중이지만 선택한 장치를 macOS 기본 출력으로 지정하지 못했습니다.";
    MSWriteStatus(@"running", routeError, _sampleRate, _preampDB,
                  _xruns.load(), 0, 0);
}

- (OSStatus)processInput:(const AudioBufferList *)inputData
                  output:(AudioBufferList *)outputData
              outputTime:(const AudioTimeStamp *)outputTime {
    if (!outputData) return noErr;
    for (UInt32 index = 0; index < outputData->mNumberBuffers; ++index) {
        AudioBuffer &buffer = outputData->mBuffers[index];
        if (buffer.mData) memset(buffer.mData, 0, buffer.mDataByteSize);
    }
    if (!_running || !inputData) return noErr;

    UInt32 frames = std::min(MSFramesForChannel(inputData, _tapInputChannelOffset),
                             MSFramesForChannel(inputData, _tapInputChannelOffset + 1));
    frames = std::min(frames, MSFramesForChannel(outputData, 0));
    frames = std::min(frames, MSFramesForChannel(outputData, 1));
    if (frames == 0 || frames > _maxFrames) {
        _xruns.fetch_add(1, std::memory_order_relaxed);
        return noErr;
    }

    float inputPeak = 0;
    for (UInt32 frame = 0; frame < frames; ++frame) {
        const float left = MSReadChannel(inputData, _tapInputChannelOffset, frame);
        const float right = MSReadChannel(inputData, _tapInputChannelOffset + 1, frame);
        _leftScratch[frame] = left;
        _rightScratch[frame] = right;
        inputPeak = std::max(inputPeak, std::max(std::abs(left), std::abs(right)));
    }
    if (_eqActive && _dsp) {
        _dsp->processPlanar(_leftScratch.data(), _rightScratch.data(), frames);
    }

    float outputPeak = 0;
    for (UInt32 frame = 0; frame < frames; ++frame) {
        const float left = static_cast<float>(_leftScratch[frame]);
        const float right = static_cast<float>(_rightScratch[frame]);
        MSWriteChannel(outputData, 0, frame, left);
        MSWriteChannel(outputData, 1, frame, right);
        _referenceRouteLeft[frame] = left;
        _referenceRouteRight[frame] = right;
        outputPeak = std::max(outputPeak, std::max(std::abs(left), std::abs(right)));
    }
    if (_echoCancellationEnabled.load(std::memory_order_relaxed) && _referenceTimeline) {
        const uint64_t hostTime = outputTime &&
            (outputTime->mFlags & kAudioTimeStampHostTimeValid)
            ? outputTime->mHostTime : mach_absolute_time();
        _referenceTimeline->push(_referenceRouteLeft.data(), _referenceRouteRight.data(),
                                 frames, _sampleRate, hostTime, MSHostTicksPerSecond());
    }
    MSAtomicMaximum(_inputPeak, inputPeak);
    MSAtomicMaximum(_outputPeak, outputPeak);
    return noErr;
}

- (void)stopRoute {
    _running = NO;
    if (_aggregateDevice != kAudioObjectUnknown && _ioProc) {
        AudioDeviceStop(_aggregateDevice, _ioProc);
        AudioDeviceDestroyIOProcID(_aggregateDevice, _ioProc);
    }
    _ioProc = nullptr;
    if (_aggregateDevice != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(_aggregateDevice);
    }
    _aggregateDevice = kAudioObjectUnknown;
    if (_tap != kAudioObjectUnknown) {
        AudioHardwareDestroyProcessTap(_tap);
    }
    _tap = kAudioObjectUnknown;
    _dsp.reset();
    _leftScratch.clear();
    _rightScratch.clear();
    _referenceRouteLeft.clear();
    _referenceRouteRight.clear();
    if (_referenceTimeline) _referenceTimeline->reset();
    _sampleRate = 0;
    _preampDB = 0;
    _eqActive = NO;
    _inputPeak.store(0);
    _outputPeak.store(0);
}

- (void)shutdown {
    [_monitorTimer invalidate];
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    if (_statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];
        _statusItem = nil;
        _statusMenu = nil;
        _outputDeviceMenu = nil;
        _echoProfileMenu = nil;
    }
    MSSelectFallbackInputIfNeeded(_micSourceUID);
    [self stopMic];
    MSMicCloseSharedMemory(_micShared);
    _micShared = nullptr;
    [self stopRoute];
    [_displayController shutdown];
    _displayController = nil;
    MSWriteStatus(@"stopped", @"", 0, 0, _xruns.load(), 0, 0);
}

@end


int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--display-self-test") == 0) {
            MTDisplayController *controller = [MTDisplayController new];
            NSDictionary *snapshot = [controller diagnosticSnapshot];
            NSData *json = [NSJSONSerialization dataWithJSONObject:snapshot
                                                           options:NSJSONWritingPrettyPrinted
                                                             error:nil];
            if (json) (void)write(STDOUT_FILENO, json.bytes, json.length);
            (void)write(STDOUT_FILENO, "\n", 1);
            return [snapshot[@"edid"][@"error"] length] == 0 ? 0 : 1;
        }
        const int singletonDescriptor = open("/tmp/io.griplabs.macsound.engine.lock",
                                             O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (singletonDescriptor < 0 ||
            flock(singletonDescriptor, LOCK_EX | LOCK_NB) != 0) {
            if (singletonDescriptor >= 0) close(singletonDescriptor);
            return 0;
        }
        (void)fchmod(singletonDescriptor, 0666);
        NSApplication *application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyProhibited];

        MSEngine *engine = [MSEngine new];
        [engine start];

        signal(SIGTERM, SIG_IGN);
        dispatch_source_t signalSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
                                                                SIGTERM, 0,
                                                                dispatch_get_main_queue());
        dispatch_source_set_event_handler(signalSource, ^{
            [engine shutdown];
            [application terminate:nil];
        });
        dispatch_resume(signalSource);
        [application run];
        close(singletonDescriptor);
    }
    return 0;
}
