#import <AppKit/AppKit.h>
#import "MicHealthUI.h"
#import <CoreAudio/CoreAudio.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "REWParser.hpp"

#include <vector>

static NSString *const MSReloadNotification = @"io.griplabs.personaltools.reload";
static NSString *const MSStopNotification = @"io.griplabs.personaltools.stop";
static NSString *const MTDisplayReinitializeNotification =
    @"io.griplabs.personaltools.display.reinitialize";
static NSString *const MTDisplaySwapNotification = @"io.griplabs.personaltools.display.swap";

static NSString *MSBaseDirectory(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/PersonalTools"];
}

static NSString *MSFiltersDirectory(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"Filters"];
}

static NSString *MSConfigPath(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"config.json"];
}

static NSString *MSStatusPath(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"status.json"];
}

static NSString *MSMicStatusPath(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"mic-status.json"];
}

static NSString *MTDisplayStatusPath(void) {
    return [MSBaseDirectory() stringByAppendingPathComponent:@"display-status.json"];
}

static BOOL MSEnsureDirectories(NSError **error) {
    return [[NSFileManager defaultManager] createDirectoryAtPath:MSFiltersDirectory()
                                     withIntermediateDirectories:YES
                                                      attributes:nil
                                                           error:error];
}

static NSMutableDictionary *MSLoadConfig(void) {
    NSData *data = [NSData dataWithContentsOfFile:MSConfigPath()];
    if (data) {
        id object = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
        if ([object isKindOfClass:[NSDictionary class]]) {
            NSMutableDictionary *config = [object mutableCopy];
            if (!config[@"echoCancellationEnabled"]) config[@"echoCancellationEnabled"] = @YES;
            config[@"echoCancellationProfile"] = @"adaptive";
            return config;
        }
    }
    return [@{@"enabled": @NO,
              @"echoCancellationEnabled": @YES,
              @"echoCancellationProfile": @"adaptive"} mutableCopy];
}

static BOOL MSSaveConfig(NSDictionary *config, NSError **error) {
    if (!MSEnsureDirectories(error)) return NO;
    NSData *data = [NSJSONSerialization dataWithJSONObject:config options:NSJSONWritingPrettyPrinted error:error];
    return data && [data writeToFile:MSConfigPath() options:NSDataWritingAtomic error:error];
}

static BOOL MSGetProperty(AudioObjectID object,
                          AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope,
                          void *data,
                          UInt32 *size) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, size, data) == noErr;
}

static NSString *MSStringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (!MSGetProperty(object, selector, kAudioObjectPropertyScopeGlobal, &value, &size) || !value) {
        return nil;
    }
    return CFBridgingRelease(value);
}

static UInt32 MSUInt32Property(AudioObjectID object, AudioObjectPropertySelector selector) {
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    MSGetProperty(object, selector, kAudioObjectPropertyScopeGlobal, &value, &size);
    return value;
}

static double MSNominalSampleRateForUID(NSString *uid) {
    if (uid.length == 0) return 0.0;
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 listSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr,
                                       &listSize) != noErr) {
        return 0.0;
    }
    std::vector<AudioObjectID> devices(listSize / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &listSize,
                                   devices.data()) != noErr) {
        return 0.0;
    }
    for (AudioObjectID deviceID : devices) {
        if (![MSStringProperty(deviceID, kAudioDevicePropertyDeviceUID) isEqualToString:uid]) {
            continue;
        }
        Float64 sampleRate = 0.0;
        UInt32 size = sizeof(sampleRate);
        return MSGetProperty(deviceID, kAudioDevicePropertyNominalSampleRate,
                             kAudioObjectPropertyScopeGlobal, &sampleRate, &size)
            ? sampleRate
            : 0.0;
    }
    return 0.0;
}

static UInt32 MSChannelCount(AudioObjectID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, scope,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<std::byte> storage(size);
    auto *list = reinterpret_cast<AudioBufferList *>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) return 0;
    UInt32 channels = 0;
    for (UInt32 index = 0; index < list->mNumberBuffers; ++index) {
        channels += list->mBuffers[index].mNumberChannels;
    }
    return channels;
}

@interface MSDevice : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *uid;
@end
@implementation MSDevice
@end

static NSArray<MSDevice *> *MSPhysicalOutputDevices(void) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
        return @[];
    }
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   devices.data()) != noErr) {
        return @[];
    }

    NSMutableArray<MSDevice *> *result = [NSMutableArray array];
    for (AudioObjectID deviceID : devices) {
        if (MSChannelCount(deviceID, kAudioDevicePropertyScopeOutput) == 0) continue;
        const UInt32 transport = MSUInt32Property(deviceID, kAudioDevicePropertyTransportType);
        if (transport == kAudioDeviceTransportTypeVirtual ||
            transport == kAudioDeviceTransportTypeAggregate) {
            continue;
        }
        NSString *uid = MSStringProperty(deviceID, kAudioDevicePropertyDeviceUID);
        NSString *name = MSStringProperty(deviceID, kAudioObjectPropertyName);
        if (uid.length == 0 || name.length == 0) continue;
        if ([uid isEqualToString:@"io.griplabs.personaltools.virtual"] ||
            [uid isEqualToString:@"EQMOutputCapture"] ||
            [name rangeOfString:@"eqMac" options:NSCaseInsensitiveSearch].location != NSNotFound) {
            continue;
        }

        MSDevice *device = [MSDevice new];
        device.name = name;
        device.uid = uid;
        [result addObject:device];
    }
    return [result sortedArrayUsingComparator:^NSComparisonResult(MSDevice *a, MSDevice *b) {
        return [a.name localizedStandardCompare:b.name];
    }];
}

@interface MSAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation MSAppDelegate {
    NSWindow *_window;
    NSPopUpButton *_devicePopup;
    NSTextField *_leftLabel;
    NSTextField *_rightLabel;
    NSTextField *_statusLabel;
    NSTextField *_detailLabel;
    NSTextField *_displayStatusLabel;
    NSTextField *_micHealthLabel;
    NSTextView *_micHealthLog;
    NSButton *_enabledButton;
    NSButton *_echoButton;
    NSMutableDictionary *_config;
    NSTimer *_statusTimer;
}

- (NSTextField *)label:(NSString *)text frame:(NSRect)frame {
    NSTextField *label = [[NSTextField alloc] initWithFrame:frame];
    label.stringValue = text;
    label.editable = NO;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.selectable = YES;
    return label;
}

- (NSButton *)button:(NSString *)title action:(SEL)action frame:(NSRect)frame {
    NSButton *button = [[NSButton alloc] initWithFrame:frame];
    button.title = title;
    button.bezelStyle = NSBezelStyleRounded;
    button.target = self;
    button.action = action;
    return button;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSError *directoryError = nil;
    MSEnsureDirectories(&directoryError);
    _config = MSLoadConfig();

    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 780)
                                          styleMask:NSWindowStyleMaskTitled |
                                                    NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Personal Tools";
    _window.delegate = self;
    [_window standardWindowButton:NSWindowCloseButton].enabled = YES;
    [_window center];

    NSView *view = _window.contentView;
    [view addSubview:[self label:@"출력 장치" frame:NSMakeRect(24, 506, 90, 24)]];
    _devicePopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(120, 502, 410, 30) pullsDown:NO];
    _devicePopup.target = self;
    _devicePopup.action = @selector(deviceChanged:);
    [view addSubview:_devicePopup];

    [view addSubview:[self button:@"L 파일 불러오기" action:@selector(importLeft:)
                            frame:NSMakeRect(24, 444, 140, 32)]];
    _leftLabel = [self label:@"선택되지 않음" frame:NSMakeRect(180, 448, 350, 24)];
    [view addSubview:_leftLabel];

    [view addSubview:[self button:@"R 파일 불러오기" action:@selector(importRight:)
                            frame:NSMakeRect(24, 396, 140, 32)]];
    _rightLabel = [self label:@"선택되지 않음" frame:NSMakeRect(180, 400, 350, 24)];
    [view addSubview:_rightLabel];

    _enabledButton = [[NSButton alloc] initWithFrame:NSMakeRect(24, 344, 130, 28)];
    _enabledButton.buttonType = NSButtonTypeSwitch;
    _enabledButton.title = @"EQ 활성화";
    _enabledButton.target = self;
    _enabledButton.action = @selector(enabledChanged:);
    [view addSubview:_enabledButton];

    [view addSubview:[self button:@"다시 로드" action:@selector(reload:)
                            frame:NSMakeRect(180, 342, 110, 30)]];
    [view addSubview:[self button:@"엔진 종료" action:@selector(stopEngine:)
                            frame:NSMakeRect(304, 342, 110, 30)]];

    _echoButton = [[NSButton alloc] initWithFrame:NSMakeRect(24, 300, 175, 28)];
    _echoButton.buttonType = NSButtonTypeSwitch;
    _echoButton.title = @"스피커 소리 제거";
    _echoButton.target = self;
    _echoButton.action = @selector(echoChanged:);
    [view addSubview:_echoButton];

    _statusLabel = [self label:@"상태: 엔진 대기 중" frame:NSMakeRect(24, 252, 506, 24)];
    _statusLabel.font = [NSFont boldSystemFontOfSize:13];
    [view addSubview:_statusLabel];
    _detailLabel = [self label:@"" frame:NSMakeRect(24, 208, 506, 42)];
    _detailLabel.lineBreakMode = NSLineBreakByWordWrapping;
    _detailLabel.maximumNumberOfLines = 2;
    [view addSubview:_detailLabel];

    NSTextField *displayHeader = [self label:@"디스플레이 도구" frame:NSMakeRect(24, 162, 200, 24)];
    displayHeader.font = [NSFont boldSystemFontOfSize:13];
    [view addSubview:displayHeader];
    [view addSubview:[self button:@"외장 디스플레이 다시 초기화  ⌃⌥⌘R"
                           action:@selector(reinitializeDisplays:)
                            frame:NSMakeRect(24, 116, 250, 32)]];
    [view addSubview:[self button:@"MO32U24 위치 스왑  ⌃⌥⌘S"
                           action:@selector(swapDisplays:)
                            frame:NSMakeRect(286, 116, 250, 32)]];
    _displayStatusLabel = [self label:@"디스플레이 상태: 엔진 대기 중"
                                      frame:NSMakeRect(24, 56, 512, 48)];
    _displayStatusLabel.lineBreakMode = NSLineBreakByWordWrapping;
    _displayStatusLabel.maximumNumberOfLines = 2;
    [view addSubview:_displayStatusLabel];

    for (NSView *existing in view.subviews) {
        NSRect frame = existing.frame;
        frame.origin.y += 220;
        existing.frame = frame;
    }
    NSTextField *healthHeader = [self label:@"마이크 경고 · 자동 복구 기록"
                                      frame:NSMakeRect(24, 218, 512, 24)];
    healthHeader.font = [NSFont boldSystemFontOfSize:13];
    [view addSubview:healthHeader];
    _micHealthLabel = [self label:@"" frame:NSMakeRect(24, 164, 512, 48)];
    _micHealthLabel.maximumNumberOfLines = 3;
    _micHealthLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [view addSubview:_micHealthLabel];
    NSScrollView *healthScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(24, 24, 512, 132)];
    healthScroll.hasVerticalScroller = YES;
    healthScroll.borderType = NSBezelBorder;
    _micHealthLog = [[NSTextView alloc] initWithFrame:healthScroll.bounds];
    _micHealthLog.editable = NO;
    _micHealthLog.selectable = YES;
    _micHealthLog.font = [NSFont systemFontOfSize:11];
    _micHealthLog.textContainerInset = NSMakeSize(8, 8);
    _micHealthLog.autoresizingMask = NSViewWidthSizable;
    _micHealthLog.textContainer.widthTracksTextView = YES;
    healthScroll.documentView = _micHealthLog;
    [view addSubview:healthScroll];

    [self refreshDevices];
    [self refreshConfigLabels];
    [self refreshStatus:nil];
    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                    target:self
                                                  selector:@selector(refreshStatus:)
                                                  userInfo:nil
                                                   repeats:YES];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    [_statusTimer invalidate];
    _statusTimer = nil;
    [NSApp terminate:nil];
}

- (void)refreshDevices {
    [_devicePopup removeAllItems];
    NSArray<MSDevice *> *devices = MSPhysicalOutputDevices();
    NSString *selectedUID = _config[@"targetDeviceUID"];
    NSInteger selectedIndex = -1;
    for (MSDevice *device in devices) {
        [_devicePopup addItemWithTitle:device.name];
        NSMenuItem *item = _devicePopup.lastItem;
        item.representedObject = device.uid;
        item.toolTip = device.uid;
        if ([device.uid isEqualToString:selectedUID]) selectedIndex = _devicePopup.numberOfItems - 1;
    }
    if (selectedIndex >= 0) [_devicePopup selectItemAtIndex:selectedIndex];
    if (_devicePopup.numberOfItems == 0) {
        [_devicePopup addItemWithTitle:@"사용 가능한 물리 출력 장치 없음"];
        _devicePopup.enabled = NO;
    }
}

- (void)refreshConfigLabels {
    NSString *leftName = _config[@"leftSourceName"];
    NSString *rightName = _config[@"rightSourceName"];
    NSNumber *leftCount = _config[@"leftBandCount"];
    NSNumber *rightCount = _config[@"rightBandCount"];
    NSNumber *leftImportedAt = _config[@"leftImportedAt"];
    NSNumber *rightImportedAt = _config[@"rightImportedAt"];
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.dateStyle = NSDateFormatterShortStyle;
    formatter.timeStyle = NSDateFormatterShortStyle;
    NSString *leftDate = leftImportedAt
        ? [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:leftImportedAt.doubleValue]]
        : nil;
    NSString *rightDate = rightImportedAt
        ? [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:rightImportedAt.doubleValue]]
        : nil;
    _leftLabel.stringValue = leftName
        ? [NSString stringWithFormat:@"%@ · %@ 밴드%@%@", leftName, leftCount ?: @0,
                                     leftDate ? @" · " : @"", leftDate ?: @""]
        : @"선택되지 않음";
    _rightLabel.stringValue = rightName
        ? [NSString stringWithFormat:@"%@ · %@ 밴드%@%@", rightName, rightCount ?: @0,
                                     rightDate ? @" · " : @"", rightDate ?: @""]
        : @"선택되지 않음";
    _enabledButton.state = [_config[@"enabled"] boolValue] ? NSControlStateValueOn
                                                           : NSControlStateValueOff;
    const BOOL echoEnabled = !_config[@"echoCancellationEnabled"] ||
        [_config[@"echoCancellationEnabled"] boolValue];
    _echoButton.state = echoEnabled ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)showError:(NSString *)message {
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"Personal Tools";
    alert.informativeText = message;
    [alert beginSheetModalForWindow:_window completionHandler:nil];
}

- (void)saveAndNotify {
    NSError *error = nil;
    if (!MSSaveConfig(_config, &error)) {
        [self showError:error.localizedDescription];
        return;
    }
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:MSReloadNotification
                                                                   object:nil
                                                                 userInfo:nil
                                                       deliverImmediately:YES];
    [self refreshConfigLabels];
}

- (void)deviceChanged:(id)sender {
    (void)sender;
    NSMenuItem *item = _devicePopup.selectedItem;
    NSString *uid = item.representedObject;
    if (![uid isKindOfClass:[NSString class]]) return;
    _config[@"targetDeviceUID"] = uid;
    _config[@"targetDeviceName"] = item.title;
    _config[@"resumeWhenTargetReturns"] = @NO;
    [self saveAndNotify];
}

- (void)importSource:(NSURL *)source channel:(personaltools::Channel)channel {
    if ([source.pathExtension caseInsensitiveCompare:@"txt"] != NSOrderedSame) {
        [self showError:@".txt 형식의 REW Configurable_PEQ 파일만 불러올 수 있습니다."];
        return;
    }
    NSString *targetUID = _devicePopup.selectedItem.representedObject;
    const double sampleRate = [targetUID isKindOfClass:[NSString class]]
        ? MSNominalSampleRateForUID(targetUID)
        : 0.0;
    if (sampleRate <= 0.0) {
        [self showError:@"먼저 사용할 출력 장치를 선택하세요."];
        return;
    }
    const auto validation = personaltools::parseREWConfigurablePEQFile(
        std::filesystem::path(source.fileSystemRepresentation), channel);
    if (!validation) {
        [self showError:[NSString stringWithUTF8String:validation.error.c_str()]];
        return;
    }
    for (const auto &filter : validation.filters) {
        if (filter.frequencyHz >= sampleRate * 0.5) {
            [self showError:[NSString stringWithFormat:
                @"%.0f Hz 필터는 선택한 장치의 Nyquist 주파수(%.0f Hz) 미만이어야 합니다.",
                filter.frequencyHz, sampleRate * 0.5]];
            return;
        }
    }
    NSError *error = nil;
    if (!MSEnsureDirectories(&error)) {
        [self showError:error.localizedDescription];
        return;
    }
    NSString *destinationName = channel == personaltools::Channel::left ? @"left.txt" : @"right.txt";
    NSString *destination = [MSFiltersDirectory() stringByAppendingPathComponent:destinationName];
    const auto parsed = personaltools::importREWConfigurablePEQFile(
        std::filesystem::path(source.fileSystemRepresentation),
        std::filesystem::path(destination.fileSystemRepresentation),
        channel);
    if (!parsed) {
        [self showError:[NSString stringWithUTF8String:parsed.error.c_str()]];
        return;
    }

    NSString *prefix = channel == personaltools::Channel::left ? @"left" : @"right";
    _config[[prefix stringByAppendingString:@"SourceName"]] = source.lastPathComponent;
    _config[[prefix stringByAppendingString:@"BandCount"]] = @(parsed.filters.size());
    _config[[prefix stringByAppendingString:@"ImportedAt"]] =
        @([[NSDate date] timeIntervalSince1970]);
    [self saveAndNotify];
}

- (void)importChannel:(personaltools::Channel)channel {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.canChooseFiles = YES;
    panel.allowsMultipleSelection = NO;
    UTType *textFileType = [UTType typeWithFilenameExtension:@"txt"];
    panel.allowedContentTypes = textFileType ? @[textFileType] : @[UTTypePlainText];
    panel.message = channel == personaltools::Channel::left
        ? @"왼쪽 채널 REW Configurable_PEQ 파일을 선택하세요."
        : @"오른쪽 채널 REW Configurable_PEQ 파일을 선택하세요.";

    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        if (response == NSModalResponseOK && panel.URL) {
            [self importSource:panel.URL channel:channel];
        }
    }];
}

- (void)importLeft:(id)sender {
    (void)sender;
    [self importChannel:personaltools::Channel::left];
}

- (void)importRight:(id)sender {
    (void)sender;
    [self importChannel:personaltools::Channel::right];
}

- (void)enabledChanged:(id)sender {
    (void)sender;
    NSMenuItem *selectedDevice = _devicePopup.selectedItem;
    if (_enabledButton.state == NSControlStateValueOn &&
        [selectedDevice.representedObject isKindOfClass:[NSString class]]) {
        _config[@"targetDeviceUID"] = selectedDevice.representedObject;
        _config[@"targetDeviceName"] = selectedDevice.title;
    }
    _config[@"enabled"] = @(_enabledButton.state == NSControlStateValueOn);
    _config[@"resumeWhenTargetReturns"] = @NO;
    [self saveAndNotify];
}

- (void)echoChanged:(id)sender {
    (void)sender;
    const BOOL enabled = _echoButton.state == NSControlStateValueOn;
    _config[@"echoCancellationEnabled"] = @(enabled);
    _config[@"echoCancellationProfile"] = @"adaptive";
    if (enabled && [_devicePopup.selectedItem.representedObject isKindOfClass:[NSString class]]) {
        _config[@"targetDeviceUID"] = _devicePopup.selectedItem.representedObject;
        _config[@"targetDeviceName"] = _devicePopup.selectedItem.title;
    }
    [self saveAndNotify];
}

- (void)reload:(id)sender {
    (void)sender;
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:MSReloadNotification
                                                                   object:nil
                                                                 userInfo:nil
                                                       deliverImmediately:YES];
}

- (void)stopEngine:(id)sender {
    (void)sender;
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:MSStopNotification
                                                                   object:nil
                                                                 userInfo:nil
                                                       deliverImmediately:YES];
}

- (void)reinitializeDisplays:(id)sender {
    (void)sender;
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:MTDisplayReinitializeNotification object:nil
                    userInfo:nil deliverImmediately:YES];
    _displayStatusLabel.stringValue = @"디스플레이 상태: 외장 디스플레이 재초기화 요청됨";
}

- (void)swapDisplays:(id)sender {
    (void)sender;
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:MTDisplaySwapNotification object:nil
                    userInfo:nil deliverImmediately:YES];
    _displayStatusLabel.stringValue = @"디스플레이 상태: MO32U24 위치 스왑 요청됨";
}

- (void)refreshDisplayStatus {
    NSData *data = [NSData dataWithContentsOfFile:MTDisplayStatusPath()];
    NSDictionary *status = data
        ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil]
        : nil;
    if (![status isKindOfClass:[NSDictionary class]]) {
        _displayStatusLabel.stringValue = @"디스플레이 상태: 엔진이 실행되지 않음";
        return;
    }
    NSDictionary<NSString *, NSString *> *actions = @{
        @"edid": @"32RTX950 EDID 유지",
        @"reinitialize": @"외장 디스플레이 재초기화",
        @"swap": @"MO32U24 역할 스왑",
        @"hotkeys": @"전역 단축키",
    };
    NSString *error = status[@"error"];
    NSString *action = actions[status[@"action"]] ?: @"준비";
    if (error.length > 0) {
        _displayStatusLabel.stringValue = [NSString stringWithFormat:@"디스플레이 상태: %@ · %@",
                                                                      action, error];
    } else if ([status[@"state"] isEqualToString:@"working"]) {
        _displayStatusLabel.stringValue = [NSString stringWithFormat:@"디스플레이 상태: %@ 진행 중", action];
    } else {
        _displayStatusLabel.stringValue = [NSString stringWithFormat:
            @"디스플레이 상태: %@ 완료 · 대상 %@대 · 변경 %@대",
            action, status[@"matched"] ?: @0, status[@"applied"] ?: @0];
    }
}

- (void)refreshStatus:(NSTimer *)timer {
    (void)timer;
    NSDictionary *health = MSLoadMicHealth();
    _micHealthLabel.stringValue = MSMicHealthSummary(health);
    _micHealthLabel.textColor = [health[@"warning"] boolValue]
        ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
    NSString *healthLog = MSMicHealthLogText(health);
    if (![_micHealthLog.string isEqualToString:healthLog]) _micHealthLog.string = healthLog;
    [self refreshDisplayStatus];
    _config = MSLoadConfig();
    _enabledButton.state = [_config[@"enabled"] boolValue] ? NSControlStateValueOn
                                                           : NSControlStateValueOff;
    const BOOL echoEnabled = !_config[@"echoCancellationEnabled"] ||
        [_config[@"echoCancellationEnabled"] boolValue];
    _echoButton.state = echoEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    NSData *data = [NSData dataWithContentsOfFile:MSStatusPath()];
    NSDictionary *status = data
        ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil]
        : nil;
    if (![status isKindOfClass:[NSDictionary class]]) {
        _statusLabel.stringValue = @"상태: 엔진이 실행되지 않음";
        _detailLabel.stringValue = @"설치 후 LaunchAgent가 엔진을 실행합니다.";
        return;
    }
    NSString *state = status[@"state"] ?: @"unknown";
    NSDictionary<NSString *, NSString *> *stateNames = @{
        @"running": @"실행 중",
        @"bypassed": @"바이패스",
        @"stopped": @"엔진 종료됨",
        @"permission required": @"시스템 오디오 권한 필요",
        @"waiting for target": @"출력 장치 선택 대기",
        @"waiting for filters": @"L/R 필터 대기",
        @"target disconnected": @"출력 장치 연결 대기",
        @"error": @"오류",
    };
    _statusLabel.stringValue = [NSString stringWithFormat:@"상태: %@",
                                                           stateNames[state] ?: state];
    NSString *error = status[@"error"];
    if (error.length > 0) {
        _detailLabel.stringValue = error;
    } else {
        NSData *micData = [NSData dataWithContentsOfFile:MSMicStatusPath()];
        NSDictionary *micStatus = micData
            ? [NSJSONSerialization JSONObjectWithData:micData options:0 error:nil]
            : nil;
        NSString *echoText = @"AEC 꺼짐";
        if ([micStatus[@"echoCancellationEnabled"] boolValue]) {
            NSDictionary<NSString *, NSString *> *echoStates = @{
                @"active": @"AEC 동작 중",
                @"double-talk": @"AEC 발화 보호",
                @"learning": @"AEC 학습 중",
                @"tracking": @"AEC 경로 재학습",
                @"input-clipping": @"M2 입력 과부하 · 입력 게인을 낮추세요",
                @"idle": @"AEC 대기",
            };
            echoText = echoStates[micStatus[@"echoCancellationState"]] ?: @"AEC 준비 중";
        }
        _detailLabel.stringValue = [NSString stringWithFormat:
            @"선택 장치 직접 출력 · %.0f Hz · preamp %.2f dB · xruns %@ · %@",
            [status[@"sampleRate"] doubleValue],
            [status[@"preampDB"] doubleValue],
            status[@"xruns"] ?: @0, echoText];
    }
}

@end

int main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication *application = [NSApplication sharedApplication];
        MSAppDelegate *delegate = [MSAppDelegate new];
        application.delegate = delegate;
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application run];
    }
    return 0;
}
