#ifndef _3DSSOUND_H_
#define _3DSSOUND_H_

#include <atomic>
#include <stdint.h>

#include "3ds.h"
#include "3dsaudio_ring.h"

// Native gpSP audio output rate (32,768 Hz stereo PCM16).
#define SND3DS_SAMPLE_RATE       32768
#define SND3DS_SAMPLES_PER_LOOP  256

// Highest Volume setting. 4 maps to 2.0x gain — about as loud as NDSP seems to go
#define SND3DS_VOLUME_MAX        4

// Sized for the max.
// Active reservoir depth is user-selectable via the "Audio Buffer" setting
// (Low/Normal/High = 4/8/16 bufs = ~31/62/125ms @ 256 frames, 32768 Hz).
#define SND3DS_WAVEBUF_MAX       16

struct SSND3DS
{
    bool                 isPlaying = false;

    // cross-thread flags written outside the lock
    std::atomic<bool>    generateSilence{false};
    std::atomic<bool>    terminateMixingThread{false};
    int         audioType = 0;              // 0 - no audio, 2 - NDSP

    short       *pcmBuffer;                 // interleaved stereo s16 frames, linearAlloc'd
    int         fillBlock;                  // which wavebuf to refill next
    int         waveBufCount;

    ndspWaveBuf waveBufs[SND3DS_WAVEBUF_MAX];

    Thread      mixingThread = NULL;

    // Protects ring-buffer access and wavebuf-queue mutations in Start/Stop.
    LightLock   audioLock;

    // Wakes the mixing thread.
    // Signaled by the NDSP frame callback (~5ms cadence per libctru)
    // and by Finalize at shutdown.
    LightEvent  ndspFrameEvent;

    StereoRingBuffer ring;
};


extern SSND3DS snd3DS;

bool snd3dsInitialize();
void snd3dsFinalize();
void snd3dsMixSamples();
void snd3dsStartPlaying();
void snd3dsStopPlaying();
void snd3dsDrainMixing();
void snd3dsResumeMixing();
void snd3dsApplyOutputVolume();

// Thread-safe ring buffer interface used by the emulation core.
void snd3dsPushSamples(const int16_t* interleavedStereo, int frameCount);
void snd3dsClearRingBuffer();

#endif
