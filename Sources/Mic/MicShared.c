#include "MicShared.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MS_LOAD_ACQUIRE(pointer) __atomic_load_n((pointer), __ATOMIC_ACQUIRE)
#define MS_LOAD_RELAXED(pointer) __atomic_load_n((pointer), __ATOMIC_RELAXED)
#define MS_STORE_RELEASE(pointer, value) __atomic_store_n((pointer), (value), __ATOMIC_RELEASE)
#define MS_STORE_RELAXED(pointer, value) __atomic_store_n((pointer), (value), __ATOMIC_RELAXED)

static void MSMicInitializeMapping(MSMicSharedMemory *shared) {
    if (MS_LOAD_ACQUIRE(&shared->magic) == MS_MIC_SHARED_MAGIC &&
        shared->version == MS_MIC_SHARED_VERSION &&
        shared->capacity == MS_MIC_RING_CAPACITY) {
        return;
    }
    memset(shared, 0, sizeof(*shared));
    shared->version = MS_MIC_SHARED_VERSION;
    shared->capacity = MS_MIC_RING_CAPACITY;
    MS_STORE_RELEASE(&shared->magic, MS_MIC_SHARED_MAGIC);
}

MSMicSharedMemory *MSMicOpenSharedMemory(void) {
    const int descriptor = open(MS_MIC_SHARED_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (descriptor < 0) return NULL;
    (void)fchmod(descriptor, 0666);
    if (ftruncate(descriptor, (off_t)sizeof(MSMicSharedMemory)) != 0) {
        close(descriptor);
        return NULL;
    }
    void *mapping = mmap(NULL, sizeof(MSMicSharedMemory), PROT_READ | PROT_WRITE,
                         MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
        close(descriptor);
        return NULL;
    }
    if (flock(descriptor, LOCK_EX) == 0) {
        MSMicInitializeMapping((MSMicSharedMemory *)mapping);
        (void)flock(descriptor, LOCK_UN);
    } else {
        munmap(mapping, sizeof(MSMicSharedMemory));
        close(descriptor);
        return NULL;
    }
    close(descriptor);
    return (MSMicSharedMemory *)mapping;
}

void MSMicCloseSharedMemory(MSMicSharedMemory *shared) {
    if (shared) munmap(shared, sizeof(*shared));
}

void MSMicResetReader(MSMicSharedMemory *shared) {
    if (!shared) return;
    MS_STORE_RELEASE(&shared->readIndex, MS_LOAD_ACQUIRE(&shared->writeIndex));
    MS_STORE_RELAXED(&shared->overruns, 0u);
    MS_STORE_RELAXED(&shared->underruns, 0u);
}

void MSMicSetEngineOnline(MSMicSharedMemory *shared, bool online) {
    if (shared) MS_STORE_RELEASE(&shared->engineOnline, online ? 1u : 0u);
}

void MSMicPublishTimestamp(MSMicSharedMemory *shared, Float64 sampleTime, uint64_t hostTime) {
    if (!shared) return;
    uint64_t bits = 0;
    memcpy(&bits, &sampleTime, sizeof(bits));
    uint64_t sequence = MS_LOAD_RELAXED(&shared->sourceSequence);
    MS_STORE_RELEASE(&shared->sourceSequence, sequence + 1u);
    MS_STORE_RELAXED(&shared->sourceSampleTimeBits, bits);
    MS_STORE_RELAXED(&shared->sourceHostTime, hostTime);
    MS_STORE_RELEASE(&shared->sourceSequence, sequence + 2u);
}

bool MSMicLoadTimestamp(const MSMicSharedMemory *shared, Float64 *sampleTime, uint64_t *hostTime) {
    if (!shared || !sampleTime || !hostTime || !MSMicEngineOnline(shared)) return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = MS_LOAD_ACQUIRE(&shared->sourceSequence);
        if ((before & 1u) != 0u) continue;
        const uint64_t bits = MS_LOAD_RELAXED(&shared->sourceSampleTimeBits);
        const uint64_t host = MS_LOAD_RELAXED(&shared->sourceHostTime);
        const uint64_t after = MS_LOAD_ACQUIRE(&shared->sourceSequence);
        if (before == after && before != 0u) {
            memcpy(sampleTime, &bits, sizeof(bits));
            *hostTime = host;
            return true;
        }
    }
    return false;
}

bool MSMicCalculateZeroTimestamp(Float64 sourceSampleTime, uint64_t sourceHostTime,
                                 Float64 periodFrames, Float64 hostTicksPerFrame,
                                 Float64 *zeroSampleTime, uint64_t *zeroHostTime) {
    if (!zeroSampleTime || !zeroHostTime || !isfinite(sourceSampleTime) ||
        sourceSampleTime < 0.0 || sourceHostTime == 0u ||
        !isfinite(periodFrames) || periodFrames <= 0.0 ||
        !isfinite(hostTicksPerFrame) || hostTicksPerFrame <= 0.0) {
        return false;
    }
    const Float64 zero = floor(sourceSampleTime / periodFrames) * periodFrames;
    const Float64 deltaTicksFloat = (sourceSampleTime - zero) * hostTicksPerFrame;
    if (!isfinite(deltaTicksFloat) || deltaTicksFloat < 0.0 ||
        deltaTicksFloat > (Float64)UINT64_MAX) {
        return false;
    }
    const uint64_t deltaTicks = (uint64_t)llround(deltaTicksFloat);
    if (deltaTicks > sourceHostTime) return false;
    *zeroSampleTime = zero;
    *zeroHostTime = sourceHostTime - deltaTicks;
    return true;
}

size_t MSMicWrite(MSMicSharedMemory *shared, const Float32 *samples, size_t frameCount) {
    if (!shared || !samples || frameCount == 0) return 0;
    const uint64_t write = MS_LOAD_RELAXED(&shared->writeIndex);
    const uint64_t read = MS_LOAD_ACQUIRE(&shared->readIndex);
    const uint64_t occupied = write >= read ? write - read : MS_MIC_RING_CAPACITY;
    const size_t writable = occupied < MS_MIC_RING_CAPACITY
        ? (size_t)(MS_MIC_RING_CAPACITY - occupied) : 0u;
    const size_t count = frameCount < writable ? frameCount : writable;
    for (size_t index = 0; index < count; ++index) {
        shared->samples[(write + index) & (MS_MIC_RING_CAPACITY - 1u)] = samples[index];
    }
    MS_STORE_RELEASE(&shared->writeIndex, write + count);
    if (count != frameCount) {
        __atomic_fetch_add(&shared->overruns, frameCount - count, __ATOMIC_RELAXED);
    }
    return count;
}

size_t MSMicRead(MSMicSharedMemory *shared, Float32 *samples, size_t frameCount,
                 size_t prebufferFrames, bool *primed) {
    if (!samples || frameCount == 0) return 0;
    if (!shared || !primed || !MSMicEngineOnline(shared)) {
        memset(samples, 0, frameCount * sizeof(*samples));
        return 0;
    }
    uint64_t read = MS_LOAD_RELAXED(&shared->readIndex);
    const uint64_t write = MS_LOAD_ACQUIRE(&shared->writeIndex);
    uint64_t available64 = write >= read ? write - read : 0u;
    if (available64 > MS_MIC_MAX_BUFFERED_FRAMES) {
        const uint64_t retained = prebufferFrames > 0
            ? prebufferFrames : MS_MIC_PREBUFFER_FRAMES;
        const uint64_t boundedRetained = retained < MS_MIC_MAX_BUFFERED_FRAMES
            ? retained : MS_MIC_MAX_BUFFERED_FRAMES;
        const uint64_t dropped = available64 - boundedRetained;
        read += dropped;
        available64 = boundedRetained;
        MS_STORE_RELEASE(&shared->readIndex, read);
        __atomic_fetch_add(&shared->overruns, dropped, __ATOMIC_RELAXED);
        *primed = false;
    }
    const size_t available = available64 > MS_MIC_RING_CAPACITY
        ? MS_MIC_RING_CAPACITY : (size_t)available64;
    if (!*primed) {
        if (available < prebufferFrames) {
            memset(samples, 0, frameCount * sizeof(*samples));
            return 0;
        }
        *primed = true;
    }
    const size_t count = frameCount < available ? frameCount : available;
    for (size_t index = 0; index < count; ++index) {
        samples[index] = shared->samples[(read + index) & (MS_MIC_RING_CAPACITY - 1u)];
    }
    if (count < frameCount) {
        memset(samples + count, 0, (frameCount - count) * sizeof(*samples));
        __atomic_fetch_add(&shared->underruns, frameCount - count, __ATOMIC_RELAXED);
        *primed = false;
    }
    MS_STORE_RELEASE(&shared->readIndex, read + count);
    return count;
}

uint64_t MSMicOverruns(const MSMicSharedMemory *shared) {
    return shared ? MS_LOAD_RELAXED(&shared->overruns) : 0u;
}

uint64_t MSMicUnderruns(const MSMicSharedMemory *shared) {
    return shared ? MS_LOAD_RELAXED(&shared->underruns) : 0u;
}

bool MSMicEngineOnline(const MSMicSharedMemory *shared) {
    return shared && MS_LOAD_ACQUIRE(&shared->engineOnline) != 0u;
}

uint64_t MSMicSourceSequence(const MSMicSharedMemory *shared) {
    return shared ? MS_LOAD_ACQUIRE(&shared->sourceSequence) : 0u;
}
