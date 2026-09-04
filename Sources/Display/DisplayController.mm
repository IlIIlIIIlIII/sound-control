#import "DisplayController.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/graphics/IOGraphicsTypes.h>
#import <objc/message.h>

#include "DisplayTools.hpp"

#include <cmath>
#include <dlfcn.h>
#include <span>
#include <string>
#include <vector>

NSString *const MTDisplayReinitializeNotification =
    @"io.griplabs.macsound.display.reinitialize";
NSString *const MTDisplaySwapNotification = @"io.griplabs.macsound.display.swap";

namespace {

using IOAVServiceRef = CFTypeRef;
using IOAVServiceCreateWithServiceFn = IOAVServiceRef (*)(CFAllocatorRef, io_service_t);
using IOAVServiceCopyEDIDFn = IOReturn (*)(IOAVServiceRef, CFDataRef*);
using IOAVServiceSetVirtualEDIDModeFn = IOReturn (*)(IOAVServiceRef, uint32_t, CFDataRef);
using ConfigureDisplayEnabledFn = CGError (*)(CGDisplayConfigRef, CGDirectDisplayID, bool);
using CreateDisplayUUIDFn = CFUUIDRef (*)(CGDirectDisplayID);
using MPDisplayInitFn = id (*)(id, SEL, int32_t);
using MPDisplayCanRotateFn = BOOL (*)(id, SEL);
using MPDisplaySetRotationFn = void (*)(id, SEL, int32_t);

NSString *MTBaseDirectory(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:
        @"Library/Application Support/MacTools"];
}

NSString *MTDisplayStatusPath(void) {
    return [MTBaseDirectory() stringByAppendingPathComponent:@"display-status.json"];
}

NSString *MTDisplayRecoveryPath(void) {
    return [MTBaseDirectory() stringByAppendingPathComponent:@"display-recovery.json"];
}

void *MTSkyLightHandle(void) {
    static void *handle = dlopen(
        "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
        RTLD_LAZY | RTLD_LOCAL);
    return handle;
}

ConfigureDisplayEnabledFn MTConfigureDisplayEnabled(void) {
    static ConfigureDisplayEnabledFn function = [] {
        void *handle = MTSkyLightHandle();
        if (!handle) return static_cast<ConfigureDisplayEnabledFn>(nullptr);
        void *symbol = dlsym(handle, "CGSConfigureDisplayEnabled");
        if (!symbol) symbol = dlsym(handle, "SLSConfigureDisplayEnabled");
        return reinterpret_cast<ConfigureDisplayEnabledFn>(symbol);
    }();
    return function;
}

void *MTMonitorPanelHandle(void) {
    static void *handle = dlopen(
        "/System/Library/PrivateFrameworks/MonitorPanel.framework/MonitorPanel",
        RTLD_LAZY | RTLD_LOCAL);
    return handle;
}

BOOL MTCanSetDisplayRotation(void) {
    if (!MTMonitorPanelHandle()) return NO;
    Class displayClass = NSClassFromString(@"MPDisplay");
    return displayClass &&
        [displayClass instancesRespondToSelector:@selector(initWithCGSDisplayID:)] &&
        [displayClass instancesRespondToSelector:@selector(canChangeOrientation)] &&
        [displayClass instancesRespondToSelector:@selector(setOrientation:)];
}

int32_t MTRotation(CGDirectDisplayID display) {
    return static_cast<int32_t>(std::lround(CGDisplayRotation(display)));
}

id MTBeginDisplayRotation(CGDirectDisplayID display, int32_t rotation) {
    if (MTRotation(display) == rotation) return [NSNull null];
    if (!MTCanSetDisplayRotation()) return nil;
    Class displayClass = NSClassFromString(@"MPDisplay");
    id allocated = [displayClass alloc];
    id mpDisplay = reinterpret_cast<MPDisplayInitFn>(objc_msgSend)(
        allocated, @selector(initWithCGSDisplayID:), static_cast<int32_t>(display));
    if (!mpDisplay ||
        !reinterpret_cast<MPDisplayCanRotateFn>(objc_msgSend)(
            mpDisplay, @selector(canChangeOrientation))) {
        return nil;
    }
    reinterpret_cast<MPDisplaySetRotationFn>(objc_msgSend)(
        mpDisplay, @selector(setOrientation:), rotation);
    return mpDisplay;
}

void MTRestoreDisplayRotations(CGDirectDisplayID firstDisplay,
                               int32_t firstRotation,
                               CGDirectDisplayID secondDisplay,
                               int32_t secondRotation) {
    id firstRequest = MTBeginDisplayRotation(firstDisplay, firstRotation);
    id secondRequest = MTBeginDisplayRotation(secondDisplay, secondRotation);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        (void)firstRequest;
        (void)secondRequest;
    });
}

void MTWaitForDisplayRotation(CGDirectDisplayID display,
                              int32_t rotation,
                              CFAbsoluteTime deadline,
                              void (^completion)(BOOL)) {
    if (MTRotation(display) == rotation) {
        completion(YES);
        return;
    }
    if (CFAbsoluteTimeGetCurrent() >= deadline) {
        completion(NO);
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        MTWaitForDisplayRotation(display, rotation, deadline, completion);
    });
}

NSData *MTDesiredEDID(NSString **error) {
    NSString *path = [[NSBundle mainBundle] pathForResource:@"32RTX950"
                                                    ofType:@"edid.base64"];
    if (!path) {
        if (error) *error = @"내장 32RTX950 EDID 리소스를 찾지 못했습니다.";
        return nil;
    }
    NSString *text = [NSString stringWithContentsOfFile:path
                                               encoding:NSUTF8StringEncoding
                                                  error:nil];
    NSData *data = [[NSData alloc] initWithBase64EncodedString:text ?: @""
                                                      options:NSDataBase64DecodingIgnoreUnknownCharacters];
    std::string validationError;
    const auto bytes = std::span<const std::byte>(
        static_cast<const std::byte *>(data.bytes), data.length);
    if (!data || !mactools::display::validateEDID(bytes, validationError) ||
        !mactools::display::isTarget32RTX950EDID(bytes)) {
        if (error) {
            *error = [NSString stringWithFormat:@"내장 EDID가 올바르지 않습니다: %s",
                                                validationError.c_str()];
        }
        return nil;
    }
    return data;
}

NSString *MTDisplayUUIDString(CGDirectDisplayID display) {
    static CreateDisplayUUIDFn createUUID = reinterpret_cast<CreateDisplayUUIDFn>(
        dlsym(RTLD_DEFAULT, "CGDisplayCreateUUIDFromDisplayID"));
    if (!createUUID) return @"";
    CFUUIDRef uuid = createUUID(display);
    if (!uuid) return @"";
    CFStringRef string = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    CFRelease(uuid);
    return CFBridgingRelease(string) ?: @"";
}

NSArray<NSNumber *> *MTOrderedOnlineExternalDisplayIDs(BOOL *hasBuiltIn) {
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0) {
        if (hasBuiltIn) *hasBuiltIn = NO;
        return @[];
    }
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetOnlineDisplayList(count, ids.data(), &count) != kCGErrorSuccess) {
        if (hasBuiltIn) *hasBuiltIn = NO;
        return @[];
    }
    BOOL builtin = NO;
    std::vector<mactools::display::ReinitializeDisplay> external;
    for (uint32_t index = 0; index < count; ++index) {
        const CGDirectDisplayID display = ids[index];
        if (CGDisplayIsBuiltin(display)) {
            if (CGDisplayIsActive(display)) builtin = YES;
        } else if (CGDisplayIsOnline(display) && CGDisplayVendorNumber(display) != 0) {
            NSString *uuid = MTDisplayUUIDString(display);
            external.push_back({display,
                                CGDisplayVendorNumber(display),
                                CGDisplayModelNumber(display),
                                uuid.UTF8String ?: ""});
        }
    }
    if (hasBuiltIn) *hasBuiltIn = builtin;
    NSMutableArray<NSNumber *> *ordered = [NSMutableArray array];
    for (const auto display : mactools::display::makeReinitializeOrder(
             std::move(external))) {
        [ordered addObject:@(display)];
    }
    return ordered;
}

void MTRequestDisplayProbe(void) {
    const char *classes[] = {"IOFramebuffer", "IOMobileFramebuffer"};
    for (const char *className : classes) {
        io_iterator_t iterator = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                         IOServiceMatching(className),
                                         &iterator) != KERN_SUCCESS) {
            continue;
        }
        io_service_t service = IO_OBJECT_NULL;
        while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
            (void)IOServiceRequestProbe(service, kIOFBUserRequestProbe);
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
    }
}

std::vector<mactools::display::DisplayGeometry> MTDisplayGeometries(void) {
    uint32_t count = 0;
    std::vector<mactools::display::DisplayGeometry> result;
    if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0) {
        return result;
    }
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetOnlineDisplayList(count, ids.data(), &count) != kCGErrorSuccess) return result;
    result.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const CGDirectDisplayID display = ids[index];
        const CGRect bounds = CGDisplayBounds(display);
        result.push_back({
            display,
            CGDisplayVendorNumber(display),
            CGDisplayModelNumber(display),
            CGDisplaySerialNumber(display),
            static_cast<int32_t>(bounds.origin.x),
            static_cast<int32_t>(bounds.origin.y),
            static_cast<uint32_t>(bounds.size.width),
            static_cast<uint32_t>(bounds.size.height),
            MTRotation(display),
            CGDisplayIsBuiltin(display) != 0,
            CGDisplayIsOnline(display) != 0,
            CGDisplayMirrorsDisplay(display) != kCGNullDirectDisplay,
        });
    }
    return result;
}

}  // namespace

@interface MTDisplayController ()
- (void)applyEDIDIfNeeded;
- (void)reinitializeExternalDisplays;
- (void)swapMO32U24Displays;
- (void)scheduleEDIDMaintenance;
- (void)enableDisplays:(NSArray<NSNumber *> *)ids
               atIndex:(NSUInteger)index
            completion:(void (^)(BOOL success))completion;
@end

static OSStatus MTHotKeyHandler(EventHandlerCallRef nextHandler,
                                EventRef event,
                                void *context) {
    (void)nextHandler;
    EventHotKeyID hotKey{};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                          nullptr, sizeof(hotKey), nullptr, &hotKey) != noErr) {
        return eventNotHandledErr;
    }
    MTDisplayController *controller = (__bridge MTDisplayController *)context;
    if (hotKey.signature != 'MTDS') return eventNotHandledErr;
    if (hotKey.id == 1) {
        [controller reinitializeExternalDisplays];
        return noErr;
    }
    if (hotKey.id == 2) {
        [controller swapMO32U24Displays];
        return noErr;
    }
    return eventNotHandledErr;
}

static void MTDisplayReconfigurationCallback(CGDirectDisplayID display,
                                             CGDisplayChangeSummaryFlags flags,
                                             void *context) {
    (void)display;
    if ((flags & kCGDisplayBeginConfigurationFlag) != 0) return;
    MTDisplayController *controller = (__bridge MTDisplayController *)context;
    dispatch_async(dispatch_get_main_queue(), ^{
        [controller scheduleEDIDMaintenance];
    });
}

@implementation MTDisplayController {
    EventHandlerRef _hotKeyHandler;
    EventHotKeyRef _reinitializeHotKey;
    EventHotKeyRef _swapHotKey;
    BOOL _started;
    BOOL _edidScheduled;
    BOOL _reinitializing;
    BOOL _swapping;
    BOOL _hotKeysReady;
}

- (void)writeStatus:(NSString *)state
              action:(NSString *)action
               error:(NSString *)error
             matched:(NSUInteger)matched
             applied:(NSUInteger)applied {
    [[NSFileManager defaultManager] createDirectoryAtPath:MTBaseDirectory()
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSDictionary *status = @{
        @"state": state ?: @"unknown",
        @"action": action ?: @"",
        @"error": error ?: @"",
        @"matched": @(matched),
        @"applied": @(applied),
        @"hotkeysReady": @(_hotKeysReady),
        @"updatedAt": @([[NSDate date] timeIntervalSince1970]),
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:status options:0 error:nil];
    [data writeToFile:MTDisplayStatusPath() options:NSDataWritingAtomic error:nil];
}

- (BOOL)setDisplays:(NSArray<NSNumber *> *)ids enabled:(BOOL)enabled {
    ConfigureDisplayEnabledFn configure = MTConfigureDisplayEnabled();
    if (!configure) return NO;
    BOOL success = YES;
    for (NSNumber *number in ids) {
        CGDisplayConfigRef config = nullptr;
        CGError status = CGBeginDisplayConfiguration(&config);
        if (status == kCGErrorSuccess) {
            status = configure(config, number.unsignedIntValue, enabled);
        }
        if (status == kCGErrorSuccess) {
            status = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
        } else if (config) {
            CGCancelDisplayConfiguration(config);
        }
        success = success && status == kCGErrorSuccess;
    }
    return success;
}

- (void)enableDisplays:(NSArray<NSNumber *> *)ids
               atIndex:(NSUInteger)index
            completion:(void (^)(BOOL success))completion {
    if (index >= ids.count) {
        completion(YES);
        return;
    }
    if (![self setDisplays:@[ids[index]] enabled:YES]) {
        completion(NO);
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        [self enableDisplays:ids atIndex:index + 1 completion:completion];
    });
}

- (void)recoverDisplaysIfNeeded {
    NSData *data = [NSData dataWithContentsOfFile:MTDisplayRecoveryPath()];
    NSArray *ids = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![ids isKindOfClass:[NSArray class]] || ids.count == 0) return;
    if ([self setDisplays:ids enabled:YES]) {
        [[NSFileManager defaultManager] removeItemAtPath:MTDisplayRecoveryPath() error:nil];
        MTRequestDisplayProbe();
    } else {
        [self writeStatus:@"error" action:@"reinitialize"
                    error:@"이전 디스플레이 재초기화의 복구를 완료하지 못했습니다."
                  matched:0 applied:0];
    }
}

- (void)start {
    if (_started) return;
    _started = YES;
    [self recoverDisplaysIfNeeded];

    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self selector:@selector(reinitializeNotification:)
               name:MTDisplayReinitializeNotification object:nil];
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self selector:@selector(swapNotification:)
               name:MTDisplaySwapNotification object:nil];
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserver:self selector:@selector(wakeNotification:)
               name:NSWorkspaceDidWakeNotification object:nil];
    CGDisplayRegisterReconfigurationCallback(MTDisplayReconfigurationCallback,
                                              (__bridge void *)self);

    EventTypeSpec eventType{kEventClassKeyboard, kEventHotKeyPressed};
    OSStatus handlerStatus = InstallEventHandler(GetApplicationEventTarget(),
                                                  MTHotKeyHandler, 1, &eventType,
                                                  (__bridge void *)self,
                                                  &_hotKeyHandler);
    EventHotKeyID reinitializeID{'MTDS', 1};
    EventHotKeyID swapID{'MTDS', 2};
    OSStatus reinitializeStatus = RegisterEventHotKey(kVK_ANSI_R,
        controlKey | optionKey | cmdKey, reinitializeID,
        GetApplicationEventTarget(), 0, &_reinitializeHotKey);
    OSStatus swapStatus = RegisterEventHotKey(kVK_ANSI_S,
        controlKey | optionKey | cmdKey, swapID,
        GetApplicationEventTarget(), 0, &_swapHotKey);
    _hotKeysReady = handlerStatus == noErr &&
                    reinitializeStatus == noErr && swapStatus == noErr;
    if (!_hotKeysReady) {
        [self writeStatus:@"error" action:@"hotkeys"
                    error:@"디스플레이 전역 단축키를 등록하지 못했습니다."
                  matched:0 applied:0];
    }
    [self scheduleEDIDMaintenance];
}

- (void)shutdown {
    if (!_started) return;
    _started = NO;
    CGDisplayRemoveReconfigurationCallback(MTDisplayReconfigurationCallback,
                                            (__bridge void *)self);
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
    if (_reinitializeHotKey) UnregisterEventHotKey(_reinitializeHotKey);
    if (_swapHotKey) UnregisterEventHotKey(_swapHotKey);
    if (_hotKeyHandler) RemoveEventHandler(_hotKeyHandler);
    _reinitializeHotKey = nullptr;
    _swapHotKey = nullptr;
    _hotKeyHandler = nullptr;
}

- (void)reinitializeNotification:(NSNotification *)notification {
    (void)notification;
    [self reinitializeExternalDisplays];
}

- (void)swapNotification:(NSNotification *)notification {
    (void)notification;
    [self swapMO32U24Displays];
}

- (void)wakeNotification:(NSNotification *)notification {
    (void)notification;
    [self scheduleEDIDMaintenance];
}

- (void)scheduleEDIDMaintenance {
    if (!_started || _edidScheduled || _reinitializing || _swapping) return;
    _edidScheduled = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        self->_edidScheduled = NO;
        if (self->_started && !self->_reinitializing && !self->_swapping) {
            [self applyEDIDIfNeeded];
        }
    });
}

- (NSDictionary *)inspectEDIDApplying:(BOOL)apply {
    NSString *resourceError = nil;
    NSData *desired = MTDesiredEDID(&resourceError);
    if (!desired) return @{@"error": resourceError ?: @"EDID resource error"};

    void *framework = dlopen(
        "/System/Library/Frameworks/IOKit.framework/IOKit",
        RTLD_LAZY | RTLD_LOCAL);
    auto create = framework ? reinterpret_cast<IOAVServiceCreateWithServiceFn>(
        dlsym(framework, "IOAVServiceCreateWithService")) : nullptr;
    auto copy = framework ? reinterpret_cast<IOAVServiceCopyEDIDFn>(
        dlsym(framework, "IOAVServiceCopyEDID")) : nullptr;
    auto set = framework ? reinterpret_cast<IOAVServiceSetVirtualEDIDModeFn>(
        dlsym(framework, "IOAVServiceSetVirtualEDIDMode")) : nullptr;
    if (!create || !copy || !set) {
        return @{@"error": @"이 macOS에서 EDID 유지 기능을 사용할 수 없습니다."};
    }

    NSUInteger matched = 0;
    NSUInteger applied = 0;
    NSUInteger failures = 0;
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(
        kIOMainPortDefault, IOServiceMatching("DCPAVServiceProxy"), &iterator);
    if (result != KERN_SUCCESS) {
        return @{@"error": @"외장 디스플레이 서비스를 열지 못했습니다."};
    }
    io_service_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFTypeRef location = IORegistryEntryCreateCFProperty(
            service, CFSTR("Location"), kCFAllocatorDefault, 0);
        const BOOL embedded = location && CFGetTypeID(location) == CFStringGetTypeID() &&
            CFStringCompare(static_cast<CFStringRef>(location), CFSTR("Embedded"),
                            kCFCompareCaseInsensitive) == kCFCompareEqualTo;
        if (location) CFRelease(location);
        if (embedded) {
            IOObjectRelease(service);
            continue;
        }

        IOAVServiceRef avService = create(kCFAllocatorDefault, service);
        CFDataRef current = nullptr;
        if (avService && copy(avService, &current) == kIOReturnSuccess && current) {
            const auto bytes = std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(CFDataGetBytePtr(current)),
                static_cast<std::size_t>(CFDataGetLength(current)));
            const BOOL exact = [(__bridge NSData *)current isEqualToData:desired];
            if (exact || mactools::display::isTarget32RTX950EDID(bytes)) {
                ++matched;
                if (apply && !exact) {
                    if (set(avService, 1, (__bridge CFDataRef)desired) == kIOReturnSuccess) {
                        ++applied;
                    } else {
                        ++failures;
                    }
                }
            }
        }
        if (current) CFRelease(current);
        if (avService) CFRelease(avService);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return @{
        @"matched": @(matched), @"applied": @(applied), @"failures": @(failures),
        @"error": failures ? @"일부 32RTX950에 EDID를 적용하지 못했습니다." : @"",
    };
}

- (void)applyEDIDIfNeeded {
    NSDictionary *result = [self inspectEDIDApplying:YES];
    NSString *error = result[@"error"] ?: @"";
    [self writeStatus:error.length ? @"error" : @"ready"
                action:@"edid"
                 error:error
               matched:[result[@"matched"] unsignedIntegerValue]
               applied:[result[@"applied"] unsignedIntegerValue]];
}

- (void)reinitializeExternalDisplays {
    if (_reinitializing) return;
    MTRequestDisplayProbe();
    BOOL hasBuiltIn = NO;
    NSArray<NSNumber *> *external = MTOrderedOnlineExternalDisplayIDs(&hasBuiltIn);
    if (!hasBuiltIn) {
        [self writeStatus:@"error" action:@"reinitialize"
                    error:@"안전한 복구를 위해 활성 내장 디스플레이가 필요합니다."
                  matched:0 applied:0];
        return;
    }
    if (external.count == 0) {
        [self writeStatus:@"error" action:@"reinitialize"
                    error:@"현재 온라인인 외장 디스플레이가 없습니다."
                  matched:0 applied:0];
        return;
    }
    if (!MTConfigureDisplayEnabled()) {
        [self writeStatus:@"error" action:@"reinitialize"
                    error:@"이 macOS에서 외장 디스플레이 재초기화를 사용할 수 없습니다."
                  matched:0 applied:0];
        return;
    }

    [[NSFileManager defaultManager] createDirectoryAtPath:MTBaseDirectory()
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSData *recovery = [NSJSONSerialization dataWithJSONObject:external options:0 error:nil];
    [recovery writeToFile:MTDisplayRecoveryPath() options:NSDataWritingAtomic error:nil];
    _reinitializing = YES;
    NSArray<NSNumber *> *teardownOrder = external.reverseObjectEnumerator.allObjects;
    if (![self setDisplays:teardownOrder enabled:NO]) {
        _reinitializing = NO;
        (void)[self setDisplays:external enabled:YES];
        [self writeStatus:@"error" action:@"reinitialize"
                    error:@"외장 디스플레이 연결을 내리지 못했습니다."
                  matched:0 applied:0];
        return;
    }
    [self writeStatus:@"working" action:@"reinitialize" error:@""
                matched:external.count applied:0];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 800 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        [self enableDisplays:external atIndex:0 completion:^(BOOL restored) {
            self->_reinitializing = NO;
            if (restored) {
                [[NSFileManager defaultManager]
                    removeItemAtPath:MTDisplayRecoveryPath() error:nil];
                MTRequestDisplayProbe();
                [self writeStatus:@"ready" action:@"reinitialize" error:@""
                            matched:external.count applied:external.count];
                [self scheduleEDIDMaintenance];
            } else {
                [self writeStatus:@"error" action:@"reinitialize"
                            error:@"일부 외장 디스플레이 복구에 실패했습니다. 엔진을 다시 시작하면 재시도합니다."
                          matched:external.count applied:0];
            }
        }];
    });
}

- (void)swapMO32U24Displays {
    if (_swapping || _reinitializing) return;
    std::string planError;
    auto plan = mactools::display::makeMO32U24SwapPlan(MTDisplayGeometries(), planError);
    if (!plan) {
        [self writeStatus:@"error" action:@"swap"
                    error:[NSString stringWithUTF8String:planError.c_str()]
                  matched:0 applied:0];
        return;
    }
    if (!MTCanSetDisplayRotation()) {
        [self writeStatus:@"error" action:@"swap"
                    error:@"이 macOS에서 MO32U24 회전 역할을 교환할 수 없습니다."
                  matched:2 applied:0];
        return;
    }

    const auto first = plan->first;
    const auto second = plan->second;
    _swapping = YES;
    [self writeStatus:@"working" action:@"swap" error:@"" matched:2 applied:0];

    id firstRotationRequest = MTBeginDisplayRotation(first.id, first.rotation);
    if (!firstRotationRequest) {
        _swapping = NO;
        [self writeStatus:@"error" action:@"swap"
                    error:@"첫 번째 MO32U24의 회전 변경을 시작하지 못했습니다."
                  matched:2 applied:0];
        return;
    }
    MTWaitForDisplayRotation(first.id, first.rotation,
                             CFAbsoluteTimeGetCurrent() + 10.0, ^(BOOL firstRotated) {
        (void)firstRotationRequest;
        id secondRotationRequest = firstRotated
            ? MTBeginDisplayRotation(second.id, second.rotation) : nil;
        if (!firstRotated || !secondRotationRequest) {
            MTRestoreDisplayRotations(first.id, second.rotation,
                                      second.id, first.rotation);
            self->_swapping = NO;
            [self writeStatus:@"error" action:@"swap"
                        error:@"MO32U24 회전 변경에 실패해 원래 회전으로 복구했습니다."
                      matched:2 applied:0];
            [self scheduleEDIDMaintenance];
            return;
        }
        MTWaitForDisplayRotation(second.id, second.rotation,
                                 CFAbsoluteTimeGetCurrent() + 10.0, ^(BOOL secondRotated) {
            (void)secondRotationRequest;
            if (!secondRotated) {
                MTRestoreDisplayRotations(first.id, second.rotation,
                                          second.id, first.rotation);
                self->_swapping = NO;
                [self writeStatus:@"error" action:@"swap"
                            error:@"MO32U24 회전 변경에 실패해 원래 회전으로 복구했습니다."
                          matched:2 applied:0];
                [self scheduleEDIDMaintenance];
                return;
            }

            CGDisplayConfigRef config = nullptr;
            CGError status = CGBeginDisplayConfiguration(&config);
            if (status == kCGErrorSuccess) {
                status = CGConfigureDisplayOrigin(config, first.id, first.x, first.y);
            }
            if (status == kCGErrorSuccess) {
                status = CGConfigureDisplayOrigin(config, second.id, second.x, second.y);
            }
            if (status == kCGErrorSuccess) {
                status = CGCompleteDisplayConfiguration(config, kCGConfigurePermanently);
            } else if (config) {
                CGCancelDisplayConfiguration(config);
            }
            if (status != kCGErrorSuccess) {
                MTRestoreDisplayRotations(first.id, second.rotation,
                                          second.id, first.rotation);
            }
            self->_swapping = NO;
            [self writeStatus:status == kCGErrorSuccess ? @"ready" : @"error"
                        action:@"swap"
                         error:status == kCGErrorSuccess
                             ? @"" : @"MO32U24 위치 적용에 실패해 회전을 복구했습니다."
                       matched:2 applied:status == kCGErrorSuccess ? 2 : 0];
            [self scheduleEDIDMaintenance];
        });
    });
}

- (NSDictionary *)diagnosticSnapshot {
    NSDictionary *edid = [self inspectEDIDApplying:NO];
    std::string planError;
    const auto geometries = MTDisplayGeometries();
    const auto swapPlan = mactools::display::makeMO32U24SwapPlan(geometries, planError);
    return @{
        @"edid": edid,
        @"onlineDisplayCount": @(geometries.size()),
        @"mo32u24SwapReady": @(swapPlan.has_value()),
        @"mo32u24SwapError": swapPlan ? @"" : [NSString stringWithUTF8String:planError.c_str()],
        @"rotationControlAvailable": @(MTCanSetDisplayRotation()),
        @"reinitializeSymbolAvailable": @(MTConfigureDisplayEnabled() != nullptr),
    };
}

@end
