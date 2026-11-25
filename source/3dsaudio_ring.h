#ifndef _3DSAUDIO_RING_H_
#define _3DSAUDIO_RING_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Lock-free-of-3DS-dependencies stereo PCM16 circular buffer.
// Callers synchronize across threads externally (e.g. LightLock in 3dssound.cpp).
struct StereoRingBuffer {
    static constexpr size_t CAPACITY_FRAMES = 8192;

    int16_t data[CAPACITY_FRAMES * 2];
    size_t readPos = 0;
    size_t writePos = 0;
    size_t count = 0;

    void reset() {
        readPos = 0;
        writePos = 0;
        count = 0;
    }

    size_t available() const {
        return count;
    }

    // Appends interleaved stereo frames. If the buffer overflows, drops the
    // oldest frames so audio latency stays bounded. Returns frames dropped.
    size_t push(const int16_t* interleavedStereo, size_t frames) {
        if (!interleavedStereo || frames == 0)
            return 0;

        size_t dropped = 0;
        if (frames >= CAPACITY_FRAMES) {
            // Keep only the most recent CAPACITY_FRAMES.
            interleavedStereo += (frames - CAPACITY_FRAMES) * 2;
            dropped = count + (frames - CAPACITY_FRAMES);
            reset();
            frames = CAPACITY_FRAMES;
        } else if (count + frames > CAPACITY_FRAMES) {
            size_t excess = count + frames - CAPACITY_FRAMES;
            readPos = (readPos + excess) % CAPACITY_FRAMES;
            count -= excess;
            dropped = excess;
        }

        size_t firstChunk = CAPACITY_FRAMES - writePos;
        if (firstChunk > frames)
            firstChunk = frames;
        memcpy(data + writePos * 2, interleavedStereo, firstChunk * 2 * sizeof(int16_t));

        size_t secondChunk = frames - firstChunk;
        if (secondChunk > 0) {
            memcpy(data, interleavedStereo + firstChunk * 2, secondChunk * 2 * sizeof(int16_t));
        }

        writePos = (writePos + frames) % CAPACITY_FRAMES;
        count += frames;
        return dropped;
    }

    // Pops up to `frames` interleaved stereo frames into `outInterleaved`.
    // Zero-fills any underrun tail. Returns the number of real frames popped.
    size_t pop(int16_t* outInterleaved, size_t frames) {
        if (!outInterleaved || frames == 0)
            return 0;

        size_t toPop = count < frames ? count : frames;
        if (toPop > 0) {
            size_t firstChunk = CAPACITY_FRAMES - readPos;
            if (firstChunk > toPop)
                firstChunk = toPop;
            memcpy(outInterleaved, data + readPos * 2, firstChunk * 2 * sizeof(int16_t));

            size_t secondChunk = toPop - firstChunk;
            if (secondChunk > 0) {
                memcpy(outInterleaved + firstChunk * 2, data, secondChunk * 2 * sizeof(int16_t));
            }

            readPos = (readPos + toPop) % CAPACITY_FRAMES;
            count -= toPop;
        }

        if (toPop < frames) {
            memset(outInterleaved + toPop * 2, 0, (frames - toPop) * 2 * sizeof(int16_t));
        }
        return toPop;
    }
};

#endif
