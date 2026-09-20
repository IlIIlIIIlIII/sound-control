#pragma once

#include <CoreAudio/CoreAudioTypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MS_MIC_SHARED_PATH "/tmp/io.griplabs.soundcontrol.mic.shared"
#define MS_MIC_SHARED_MAGIC 0x4D534D43u
#define MS_MIC_SHARED_VERSION 1u
#define MS_MIC_RING_CAPACITY 65536u
#define MS_MIC_PREBUFFER_FRAMES 960u
#define MS_MIC_MAX_BUFFERED_FRAMES 1920u

typedef struct __attribute__((aligned(64))) MSMicSharedMemory {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t reserved0;
    uint8_t padding0[48];

    uint64_t writeIndex;
    uint8_t padding1[56];
    uint64_t readIndex;
    uint8_t padding2[56];
    uint64_t overruns;
    uint8_t padding3[56];
    uint64_t underruns;
    uint8_t padding4[56];
    uint64_t sourceSampleTimeBits;
    uint64_t sourceHostTime;
    uint64_t sourceSequence;
    uint32_t engineOnline;
    uint32_t reserved1;
    uint8_t padding5[24];

    Float32 samples[MS_MIC_RING_CAPACITY];
} MSMicSharedMemory;

MSMicSharedMemory *MSMicOpenSharedMemory(void);
void MSMicCloseSharedMemory(MSMicSharedMemory *shared);
void MSMicResetReader(MSMicSharedMemory *shared);
void MSMicSetEngineOnline(MSMicSharedMemory *shared, bool online);
void MSMicPublishTimestamp(MSMicSharedMemory *shared, Float64 sampleTime, uint64_t hostTime);
bool MSMicLoadTimestamp(const MSMicSharedMemory *shared, Float64 *sampleTime, uint64_t *hostTime);
bool MSMicCalculateZeroTimestamp(Float64 sourceSampleTime, uint64_t sourceHostTime,
                                 Float64 periodFrames, Float64 hostTicksPerFrame,
                                 Float64 *zeroSampleTime, uint64_t *zeroHostTime);
size_t MSMicWrite(MSMicSharedMemory *shared, const Float32 *samples, size_t frameCount);
size_t MSMicRead(MSMicSharedMemory *shared, Float32 *samples, size_t frameCount,
                 size_t prebufferFrames, bool *primed);
uint64_t MSMicOverruns(const MSMicSharedMemory *shared);
uint64_t MSMicUnderruns(const MSMicSharedMemory *shared);
bool MSMicEngineOnline(const MSMicSharedMemory *shared);
uint64_t MSMicSourceSequence(const MSMicSharedMemory *shared);

#ifdef __cplusplus
}
#endif
