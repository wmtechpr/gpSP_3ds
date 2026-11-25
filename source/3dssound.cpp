#include <stdio.h>
#include <string.h>

#include "3ds.h"
#include "3dssound.h"
#include "3dstimer.h"
#include "3dssettings.h"
#include "3dslog.h"
#include "3dsutils.h"

SSND3DS snd3DS;

static const int waveBufCounts[3] = { 4, 8, 16 };

static inline int snd3dsWaveBufCountFromSetting()
{
    if (settings3DS.AudioBuffer < 0) settings3DS.AudioBuffer = 0;
    else if (settings3DS.AudioBuffer > 2) settings3DS.AudioBuffer = 2;

    return waveBufCounts[settings3DS.AudioBuffer];
}

void snd3dsApplyOutputVolume()
{
    if (snd3DS.audioType != 2) return;

    int v = settings3DS.UseGlobalVolume ? settings3DS.GlobalVolume : settings3DS.Volume;
    if (v < 0) v = 0;
    else if (v > SND3DS_VOLUME_MAX) v = SND3DS_VOLUME_MAX;

    float gain = 1.0f + (v * 0.25f);

    float mix[12] = { gain, gain, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ndspChnSetMix(0, mix);
}

void snd3dsPushSamples(const int16_t* interleavedStereo, int frameCount)
{
    if (snd3DS.audioType != 2 || !interleavedStereo || frameCount <= 0)
        return;

    LightLock_Lock(&snd3DS.audioLock);
    snd3DS.ring.push(interleavedStereo, (size_t)frameCount);
    LightLock_Unlock(&snd3DS.audioLock);
}

void snd3dsClearRingBuffer()
{
    LightLock_Lock(&snd3DS.audioLock);
    snd3DS.ring.reset();
    LightLock_Unlock(&snd3DS.audioLock);
}

//---------------------------------------------------------
// NDSP frame callback, runs on the DSP service thread.
// Just wakes the mixer — keeps critical sections off this thread.
//---------------------------------------------------------
static void snd3dsNdspFrameCallback(void *user)
{
    (void)user;
    LightEvent_Signal(&snd3DS.ndspFrameEvent);
}

//---------------------------------------------------------
// Refill and re-queue every free wavebuf.
//---------------------------------------------------------
void snd3dsMixSamples()
{
    LightLock_Lock(&snd3DS.audioLock);

    while (true)
    {
        ndspWaveBuf *wb = &snd3DS.waveBufs[snd3DS.fillBlock];

        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE)
            break;

        bool generateSound = snd3DS.isPlaying && !snd3DS.generateSilence;
        short *dst = (short *)wb->data_vaddr;
        int frames = SND3DS_SAMPLES_PER_LOOP;

        if (generateSound && !session3DS.turboMode)
        {
            snd3DS.ring.pop(dst, (size_t)frames);
        }
        else
        {
            if (session3DS.turboMode)
                snd3DS.ring.reset();
            memset(dst, 0, (size_t)frames * 2 * sizeof(short));
        }

        u32 size = (u32)(frames * 2 * sizeof(short));
        if (device3DS.isReal3DS)
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, size);
        else {
            // Citra has no FlushProcessDataCache SVC and spams its log with
            // "unimplemented" errors, so use DSP_FlushDataCache on emulators.
            DSP_FlushDataCache(dst, size);
        }

        ndspChnWaveBufAdd(0, wb);

        snd3DS.fillBlock = (snd3DS.fillBlock + 1) % snd3DS.waveBufCount;
    }

    LightLock_Unlock(&snd3DS.audioLock);
}

static void snd3dsMixingThread(void *p)
{
    (void)p;
    while (!snd3DS.terminateMixingThread)
    {
        LightEvent_Wait(&snd3DS.ndspFrameEvent);
        snd3dsMixSamples();
    }
}

void snd3dsStartPlaying()
{
    if (snd3DS.audioType != 2)
        return;

    LightLock_Lock(&snd3DS.audioLock);

    if (snd3DS.isPlaying)
    {
        LightLock_Unlock(&snd3DS.audioLock);
        return;
    }

    snd3DS.waveBufCount = snd3dsWaveBufCountFromSetting();
    ndspChnWaveBufClear(0);

    for (int i = 0; i < snd3DS.waveBufCount; i++)
    {
        snd3DS.waveBufs[i].status = NDSP_WBUF_DONE;
    }

    snd3DS.fillBlock = 0;
    snd3DS.ring.reset();
    snd3DS.isPlaying = true;
    snd3DS.generateSilence = false;

    LightLock_Unlock(&snd3DS.audioLock);

    LightEvent_Signal(&snd3DS.ndspFrameEvent);
}

void snd3dsStopPlaying()
{
    LightLock_Lock(&snd3DS.audioLock);

    if (!snd3DS.isPlaying)
    {
        LightLock_Unlock(&snd3DS.audioLock);
        return;
    }

    ndspChnWaveBufClear(0);

    for (int i = 0; i < snd3DS.waveBufCount; i++)
    {
        snd3DS.waveBufs[i].status = NDSP_WBUF_DONE;
    }

    snd3DS.ring.reset();
    snd3DS.isPlaying = false;

    LightLock_Unlock(&snd3DS.audioLock);
}

void snd3dsDrainMixing()
{
    snd3DS.generateSilence = true;

    LightLock_Lock(&snd3DS.audioLock);
    LightLock_Unlock(&snd3DS.audioLock);
}

void snd3dsResumeMixing()
{
    snd3DS.generateSilence = false;
}

bool snd3dsInitialize()
{
    snd3DS.isPlaying = false;
    snd3DS.audioType = 0;
    snd3DS.ring.reset();

    LightLock_Init(&snd3DS.audioLock);
    LightEvent_Init(&snd3DS.ndspFrameEvent, RESET_ONESHOT);

    Result ret = ndspInit();
    log3dsWrite("ndspInit ret = 0x%lx", (unsigned long)ret);

    if (R_FAILED(ret))
    {
        log3dsWrite("NDSP init failed — continuing without audio");
        return true;
    }

    snd3DS.audioType = 2;
    snd3DS.waveBufCount = snd3dsWaveBufCountFromSetting();

    int framesPerBuffer = SND3DS_SAMPLES_PER_LOOP;
    int bytesPerBuffer = framesPerBuffer * 2 * sizeof(short);
    int totalBytes = bytesPerBuffer * SND3DS_WAVEBUF_MAX;

    snd3DS.pcmBuffer = (short *)linearAlloc(totalBytes);
    if (!snd3DS.pcmBuffer)
    {
        log3dsWrite("NDSP linearAlloc failed (%d bytes)", totalBytes);
        ndspExit();
        snd3DS.audioType = 0;
        return false;
    }
    memset(snd3DS.pcmBuffer, 0, totalBytes);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetOutputCount(1);
    ndspSetMasterVol(1.0f);

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, (float)SND3DS_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    snd3dsApplyOutputVolume();

    memset(snd3DS.waveBufs, 0, sizeof(snd3DS.waveBufs));
    for (int i = 0; i < SND3DS_WAVEBUF_MAX; i++)
    {
        snd3DS.waveBufs[i].data_vaddr = snd3DS.pcmBuffer + (i * framesPerBuffer * 2);
        snd3DS.waveBufs[i].nsamples = framesPerBuffer;
        snd3DS.waveBufs[i].looping = false;
        snd3DS.waveBufs[i].status = NDSP_WBUF_FREE;
    }
    snd3DS.fillBlock = 0;

    log3dsWrite("NDSP ready: %d Hz stereo PCM16, %d wavebufs x %d frames",
        SND3DS_SAMPLE_RATE, snd3DS.waveBufCount, framesPerBuffer);

    snd3DS.terminateMixingThread = false;

    snd3DS.mixingThread = threadCreate(snd3dsMixingThread, NULL, 0x4000, THREAD_PRIORITY_AUDIO, device3DS.secondaryCore, false);
    if (snd3DS.mixingThread == NULL)
    {
        log3dsWrite("Unable to start mixing thread");
        snd3dsFinalize();
        return false;
    }

    LightEvent_Signal(&snd3DS.ndspFrameEvent);
    ndspSetCallback(snd3dsNdspFrameCallback, NULL);

    log3dsWrite("Mixing thread started: %lx", (unsigned long)threadGetHandle(snd3DS.mixingThread));
    return true;
}

void snd3dsFinalize()
{
    snd3DS.terminateMixingThread = true;
    LightEvent_Signal(&snd3DS.ndspFrameEvent);

    if (snd3DS.mixingThread)
    {
        log3dsWrite("join mixing thread");
        threadJoin(snd3DS.mixingThread, 1000 * 1000000);
        threadFree(snd3DS.mixingThread);
        snd3DS.mixingThread = NULL;
    }

    if (snd3DS.audioType == 2)
    {
        log3dsWrite("ndspExit");
        ndspSetCallback(NULL, NULL);
        ndspChnWaveBufClear(0);
        ndspExit();
        snd3DS.audioType = 0;
    }

    if (snd3DS.pcmBuffer)
    {
        log3dsWrite("linearFree snd3DS.pcmBuffer");
        linearFree(snd3DS.pcmBuffer);
        snd3DS.pcmBuffer = NULL;
    }
}
