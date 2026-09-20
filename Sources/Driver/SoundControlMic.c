#include "MicShared.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <math.h>
#include <string.h>

// Apple publishes NullAudio.c as the reference implementation for an AudioServerPlugIn.
// CMake downloads the pinned Apple sample and this translation unit narrows it to one input
// stream while retaining Apple's required object/property plumbing.
#include "NullAudio.c"

#define MS_MIC_DEVICE_UID "io.griplabs.soundcontrol.mic.device"
#define MS_MIC_MODEL_UID "io.griplabs.soundcontrol.mic.model"

static MSMicSharedMemory *gSoundControlMicShared = NULL;
static bool gSoundControlMicPrimed = false;
static uint32_t gSoundControlMicPresent = 0;
static dispatch_source_t gSoundControlMicPresenceTimer = NULL;
static Float64 gSoundControlMicLastZeroSampleTime = -1.0;
static UInt64 gSoundControlMicTimestampSeed = 1u;

static bool SoundControlMic_IsPresent(void) {
    return __atomic_load_n(&gSoundControlMicPresent, __ATOMIC_ACQUIRE) != 0u;
}

static void SoundControlMic_SetPresent(bool present) {
    const uint32_t value = present ? 1u : 0u;
    const uint32_t previous = __atomic_exchange_n(&gSoundControlMicPresent, value, __ATOMIC_ACQ_REL);
    if (previous == value || !gPlugIn_Host) return;

    const AudioObjectPropertyAddress alive = {
        kAudioDevicePropertyDeviceIsAlive,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_Device, 1, &alive);

    const AudioObjectPropertyAddress plugInProperties[2] = {
        {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
        {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
    };
    gPlugIn_Host->PropertiesChanged(gPlugIn_Host, kObjectID_PlugIn, 2, plugInProperties);
}

static void SoundControlMic_StartPresenceMonitor(void) {
    if (gSoundControlMicPresenceTimer) return;
    gSoundControlMicPresenceTimer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!gSoundControlMicPresenceTimer) return;

    __block uint64_t lastSequence = MSMicSourceSequence(gSoundControlMicShared);
    __block unsigned staleTicks = 0;
    dispatch_source_set_timer(gSoundControlMicPresenceTimer,
                              dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                              250 * NSEC_PER_MSEC,
                              25 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(gSoundControlMicPresenceTimer, ^{
        const uint64_t sequence = MSMicSourceSequence(gSoundControlMicShared);
        const bool online = MSMicEngineOnline(gSoundControlMicShared);
        if (online && sequence != 0u && sequence != lastSequence) {
            lastSequence = sequence;
            staleTicks = 0;
            SoundControlMic_SetPresent(true);
        } else if (!online || ++staleTicks >= 12u) {
            SoundControlMic_SetPresent(false);
        }
    });
    dispatch_resume(gSoundControlMicPresenceTimer);
}

static OSStatus SoundControlMic_Initialize(AudioServerPlugInDriverRef driver,
                                      AudioServerPlugInHostRef host) {
    gDevice_SampleRate = 48000.0;
    const OSStatus status = NullAudio_Initialize(driver, host);
    gSoundControlMicShared = MSMicOpenSharedMemory();
    SoundControlMic_StartPresenceMonitor();
    return status;
}

static Boolean SoundControlMic_HasProperty(AudioServerPlugInDriverRef driver,
                                       AudioObjectID objectID,
                                       pid_t clientPID,
                                       const AudioObjectPropertyAddress *address) {
    if (!address) return false;
    if (objectID == kObjectID_Box || objectID == kObjectID_Stream_Output ||
        objectID >= kObjectID_Volume_Input_Master) {
        return false;
    }
    if (objectID == kObjectID_Device) {
        switch (address->mSelector) {
            case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyIcon:
                return false;
            default:
                break;
        }
    }
    return NullAudio_HasProperty(driver, objectID, clientPID, address);
}

static OSStatus SoundControlMic_IsPropertySettable(AudioServerPlugInDriverRef driver,
                                               AudioObjectID objectID,
                                               pid_t clientPID,
                                               const AudioObjectPropertyAddress *address,
                                               Boolean *settable) {
    if (!address || !settable) return kAudioHardwareIllegalOperationError;
    if (!SoundControlMic_HasProperty(driver, objectID, clientPID, address)) {
        return kAudioHardwareUnknownPropertyError;
    }
    if ((objectID == kObjectID_Device &&
         address->mSelector == kAudioDevicePropertyNominalSampleRate) ||
        (objectID == kObjectID_Stream_Input &&
         (address->mSelector == kAudioStreamPropertyVirtualFormat ||
          address->mSelector == kAudioStreamPropertyPhysicalFormat))) {
        *settable = false;
        return noErr;
    }
    return NullAudio_IsPropertySettable(driver, objectID, clientPID, address, settable);
}

static OSStatus SoundControlMic_GetPropertyDataSize(AudioServerPlugInDriverRef driver,
                                                AudioObjectID objectID,
                                                pid_t clientPID,
                                                const AudioObjectPropertyAddress *address,
                                                UInt32 qualifierSize,
                                                const void *qualifier,
                                                UInt32 *dataSize) {
    if (!address || !dataSize) return kAudioHardwareIllegalOperationError;
    if (!SoundControlMic_HasProperty(driver, objectID, clientPID, address)) {
        return kAudioHardwareUnknownPropertyError;
    }
    if (objectID == kObjectID_PlugIn) {
        switch (address->mSelector) {
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList:
                *dataSize = SoundControlMic_IsPresent() ? sizeof(AudioObjectID) : 0;
                return noErr;
            case kAudioPlugInPropertyBoxList:
                *dataSize = 0;
                return noErr;
            default:
                break;
        }
    } else if (objectID == kObjectID_Device) {
        switch (address->mSelector) {
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
                *dataSize = address->mScope == kAudioObjectPropertyScopeOutput
                    ? 0 : sizeof(AudioObjectID);
                return noErr;
            case kAudioObjectPropertyControlList:
                *dataSize = 0;
                return noErr;
            case kAudioDevicePropertyAvailableNominalSampleRates:
                *dataSize = sizeof(AudioValueRange);
                return noErr;
            default:
                break;
        }
    } else if (objectID == kObjectID_Stream_Input) {
        if (address->mSelector == kAudioStreamPropertyAvailableVirtualFormats ||
            address->mSelector == kAudioStreamPropertyAvailablePhysicalFormats) {
            *dataSize = sizeof(AudioStreamRangedDescription);
            return noErr;
        }
    }
    return NullAudio_GetPropertyDataSize(driver, objectID, clientPID, address,
                                         qualifierSize, qualifier, dataSize);
}

static OSStatus SoundControlMic_Put(UInt32 required, UInt32 supplied, UInt32 *written,
                               void *destination, const void *source) {
    if (!written || !destination || supplied < required) return kAudioHardwareBadPropertySizeError;
    memcpy(destination, source, required);
    *written = required;
    return noErr;
}

static AudioStreamBasicDescription SoundControlMic_Format(void) {
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = sizeof(Float32);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(Float32);
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;
    return format;
}

static OSStatus SoundControlMic_GetPropertyData(AudioServerPlugInDriverRef driver,
                                            AudioObjectID objectID,
                                            pid_t clientPID,
                                            const AudioObjectPropertyAddress *address,
                                            UInt32 qualifierSize,
                                            const void *qualifier,
                                            UInt32 dataSize,
                                            UInt32 *written,
                                            void *data) {
    if (!address || !written || !data) return kAudioHardwareIllegalOperationError;
    if (!SoundControlMic_HasProperty(driver, objectID, clientPID, address)) {
        return kAudioHardwareUnknownPropertyError;
    }
    if (objectID == kObjectID_PlugIn) {
        switch (address->mSelector) {
            case kAudioObjectPropertyManufacturer: {
                CFStringRef value = CFSTR("SoundControl");
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList: {
                if (!SoundControlMic_IsPresent()) {
                    *written = 0;
                    return noErr;
                }
                const AudioObjectID value = kObjectID_Device;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioPlugInPropertyBoxList:
                *written = 0;
                return noErr;
            case kAudioPlugInPropertyTranslateUIDToDevice: {
                if (qualifierSize != sizeof(CFStringRef) || !qualifier) {
                    return kAudioHardwareBadPropertySizeError;
                }
                const CFStringRef uid = *(const CFStringRef *)qualifier;
                const AudioObjectID value = SoundControlMic_IsPresent() && uid &&
                    CFStringCompare(uid, CFSTR(MS_MIC_DEVICE_UID), 0) == kCFCompareEqualTo
                    ? kObjectID_Device : kAudioObjectUnknown;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            default:
                break;
        }
    } else if (objectID == kObjectID_Device) {
        switch (address->mSelector) {
            case kAudioObjectPropertyName: {
                CFStringRef value = CFSTR("SoundControl Mic");
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioObjectPropertyManufacturer: {
                CFStringRef value = CFSTR("SoundControl");
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioObjectPropertyElementName: {
                CFStringRef value = address->mElement == kAudioObjectPropertyElementMain
                    ? CFSTR("M2 Input 1") : CFSTR("Input 1");
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioDevicePropertyDeviceUID: {
                CFStringRef value = CFSTR(MS_MIC_DEVICE_UID);
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioDevicePropertyModelUID: {
                CFStringRef value = CFSTR(MS_MIC_MODEL_UID);
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams: {
                if (address->mScope == kAudioObjectPropertyScopeOutput) {
                    *written = 0;
                    return noErr;
                }
                const AudioObjectID value = kObjectID_Stream_Input;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioObjectPropertyControlList:
                *written = 0;
                return noErr;
            case kAudioDevicePropertyNominalSampleRate: {
                const Float64 value = 48000.0;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                const AudioValueRange value = {48000.0, 48000.0};
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: {
                const UInt32 value = address->mScope == kAudioObjectPropertyScopeInput ? 1u : 0u;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: {
                const UInt32 value = 0u;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioDevicePropertyDeviceIsAlive: {
                const UInt32 value = SoundControlMic_IsPresent() ? 1u : 0u;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            default:
                break;
        }
    } else if (objectID == kObjectID_Stream_Input) {
        switch (address->mSelector) {
            case kAudioObjectPropertyName: {
                CFStringRef value = CFSTR("M2 Input 1");
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                const AudioStreamBasicDescription value = SoundControlMic_Format();
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription value = {0};
                value.mFormat = SoundControlMic_Format();
                value.mSampleRateRange.mMinimum = 48000.0;
                value.mSampleRateRange.mMaximum = 48000.0;
                return SoundControlMic_Put(sizeof(value), dataSize, written, data, &value);
            }
            default:
                break;
        }
    }
    return NullAudio_GetPropertyData(driver, objectID, clientPID, address, qualifierSize,
                                     qualifier, dataSize, written, data);
}

static OSStatus SoundControlMic_SetPropertyData(AudioServerPlugInDriverRef driver,
                                            AudioObjectID objectID,
                                            pid_t clientPID,
                                            const AudioObjectPropertyAddress *address,
                                            UInt32 qualifierSize,
                                            const void *qualifier,
                                            UInt32 dataSize,
                                            const void *data) {
    if (!address || !data) return kAudioHardwareIllegalOperationError;
    if ((objectID == kObjectID_Device &&
         address->mSelector == kAudioDevicePropertyNominalSampleRate) ||
        (objectID == kObjectID_Stream_Input &&
         (address->mSelector == kAudioStreamPropertyVirtualFormat ||
          address->mSelector == kAudioStreamPropertyPhysicalFormat))) {
        return kAudioHardwareUnsupportedOperationError;
    }
    return NullAudio_SetPropertyData(driver, objectID, clientPID, address, qualifierSize,
                                     qualifier, dataSize, data);
}

static OSStatus SoundControlMic_StartIO(AudioServerPlugInDriverRef driver,
                                    AudioObjectID deviceID,
                                    UInt32 clientID) {
    if (!gSoundControlMicShared) gSoundControlMicShared = MSMicOpenSharedMemory();
    MSMicResetReader(gSoundControlMicShared);
    gSoundControlMicPrimed = false;
    pthread_mutex_lock(&gDevice_IOMutex);
    gSoundControlMicLastZeroSampleTime = -1.0;
    ++gSoundControlMicTimestampSeed;
    pthread_mutex_unlock(&gDevice_IOMutex);
    return NullAudio_StartIO(driver, deviceID, clientID);
}

static OSStatus SoundControlMic_GetZeroTimeStamp(AudioServerPlugInDriverRef driver,
                                             AudioObjectID deviceID,
                                             UInt32 clientID,
                                             Float64 *sampleTime,
                                             UInt64 *hostTime,
                                             UInt64 *seed) {
    if (driver != gAudioServerPlugInDriverRef || deviceID != kObjectID_Device) {
        return kAudioHardwareBadObjectError;
    }
    if (!sampleTime || !hostTime || !seed) return kAudioHardwareIllegalOperationError;

    Float64 sourceSampleTime = 0.0;
    uint64_t sourceHostTime = 0u;
    Float64 zeroSampleTime = 0.0;
    uint64_t zeroHostTime = 0u;
    if (MSMicLoadTimestamp(gSoundControlMicShared, &sourceSampleTime, &sourceHostTime) &&
        MSMicCalculateZeroTimestamp(sourceSampleTime, sourceHostTime,
                                    (Float64)kDevice_RingBufferSize,
                                    gDevice_HostTicksPerFrame,
                                    &zeroSampleTime, &zeroHostTime)) {
        pthread_mutex_lock(&gDevice_IOMutex);
        if (gSoundControlMicLastZeroSampleTime >= 0.0 &&
            zeroSampleTime < gSoundControlMicLastZeroSampleTime) {
            ++gSoundControlMicTimestampSeed;
        }
        gSoundControlMicLastZeroSampleTime = zeroSampleTime;
        *sampleTime = zeroSampleTime;
        *hostTime = zeroHostTime;
        *seed = gSoundControlMicTimestampSeed;
        pthread_mutex_unlock(&gDevice_IOMutex);
        return noErr;
    }

    const OSStatus status = NullAudio_GetZeroTimeStamp(
        driver, deviceID, clientID, sampleTime, hostTime, seed);
    if (status == noErr) {
        pthread_mutex_lock(&gDevice_IOMutex);
        if (gSoundControlMicLastZeroSampleTime >= 0.0) {
            gSoundControlMicLastZeroSampleTime = -1.0;
            ++gSoundControlMicTimestampSeed;
        }
        *seed = gSoundControlMicTimestampSeed;
        pthread_mutex_unlock(&gDevice_IOMutex);
    }
    return status;
}

static OSStatus SoundControlMic_WillDoIOOperation(AudioServerPlugInDriverRef driver,
                                             AudioObjectID deviceID,
                                             UInt32 clientID,
                                             UInt32 operationID,
                                             Boolean *willDo,
                                             Boolean *willDoInPlace) {
    (void)clientID;
    if (driver != gAudioServerPlugInDriverRef || deviceID != kObjectID_Device) {
        return kAudioHardwareBadObjectError;
    }
    if (!willDo || !willDoInPlace) return kAudioHardwareIllegalOperationError;
    *willDo = operationID == kAudioServerPlugInIOOperationReadInput;
    *willDoInPlace = true;
    return noErr;
}

static OSStatus SoundControlMic_DoIOOperation(AudioServerPlugInDriverRef driver,
                                         AudioObjectID deviceID,
                                         AudioObjectID streamID,
                                         UInt32 clientID,
                                         UInt32 operationID,
                                         UInt32 frameCount,
                                         const AudioServerPlugInIOCycleInfo *cycleInfo,
                                         void *mainBuffer,
                                         void *secondaryBuffer) {
    (void)clientID;
    (void)cycleInfo;
    (void)secondaryBuffer;
    if (driver != gAudioServerPlugInDriverRef || deviceID != kObjectID_Device ||
        streamID != kObjectID_Stream_Input) {
        return kAudioHardwareBadObjectError;
    }
    if (operationID == kAudioServerPlugInIOOperationReadInput && mainBuffer) {
        (void)MSMicRead(gSoundControlMicShared, (Float32 *)mainBuffer, frameCount,
                        MS_MIC_PREBUFFER_FRAMES, &gSoundControlMicPrimed);
    }
    return noErr;
}

void *SoundControlMic_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID) {
    gAudioServerPlugInDriverInterface.Initialize = SoundControlMic_Initialize;
    gAudioServerPlugInDriverInterface.HasProperty = SoundControlMic_HasProperty;
    gAudioServerPlugInDriverInterface.IsPropertySettable = SoundControlMic_IsPropertySettable;
    gAudioServerPlugInDriverInterface.GetPropertyDataSize = SoundControlMic_GetPropertyDataSize;
    gAudioServerPlugInDriverInterface.GetPropertyData = SoundControlMic_GetPropertyData;
    gAudioServerPlugInDriverInterface.SetPropertyData = SoundControlMic_SetPropertyData;
    gAudioServerPlugInDriverInterface.StartIO = SoundControlMic_StartIO;
    gAudioServerPlugInDriverInterface.GetZeroTimeStamp = SoundControlMic_GetZeroTimeStamp;
    gAudioServerPlugInDriverInterface.WillDoIOOperation = SoundControlMic_WillDoIOOperation;
    gAudioServerPlugInDriverInterface.DoIOOperation = SoundControlMic_DoIOOperation;
    return NullAudio_Create(allocator, requestedTypeUUID);
}
