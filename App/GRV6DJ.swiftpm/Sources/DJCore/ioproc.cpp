// CoreAudio RemoteIO output. Kept in C++ so no Swift runs on the audio thread.
// The client format is fixed at the engine sample rate; RemoteIO converts to the hardware rate.
#include "djcore.h"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>

static OSStatus renderCb(void *refCon, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames,
                         AudioBufferList *abl) {
    float *outs[16];
    int n = int(abl->mNumberBuffers);
    if (n > 16) n = 16;
    for (int i = 0; i < n; ++i) outs[i] = static_cast<float *>(abl->mBuffers[i].mData);
    djc_render(static_cast<djc_engine *>(refCon), outs, n, int32_t(frames));
    return noErr;
}

extern "C" int32_t djc_ioproc(void *refCon, void *flags, const void *ts, uint32_t bus, uint32_t frames, void *ioData) {
    return renderCb(refCon, static_cast<AudioUnitRenderActionFlags *>(flags), static_cast<const AudioTimeStamp *>(ts),
                    bus, frames, static_cast<AudioBufferList *>(ioData));
}

extern "C" void *djc_io_start(djc_engine *e, int32_t channels, int32_t *status) {
    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    AudioUnit unit = nullptr;
    OSStatus s = AudioComponentInstanceNew(comp, &unit);
    if (s != noErr) { if (status) *status = int32_t(s); return nullptr; }
    AudioStreamBasicDescription f{};
    f.mSampleRate = djc_sample_rate(e);
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = UInt32(channels);
    f.mBitsPerChannel = 32;
    s = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (s == noErr) {
        UInt32 maxFrames = 4096;
        s = AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames,
                                 sizeof maxFrames);
    }
    if (s == noErr) {
        AURenderCallbackStruct cb{renderCb, e};
        s = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    }
    if (s == noErr) s = AudioUnitInitialize(unit);
    if (s == noErr) s = AudioOutputUnitStart(unit);
    if (s != noErr) {
        AudioComponentInstanceDispose(unit);
        if (status) *status = int32_t(s);
        return nullptr;
    }
    if (status) *status = 0;
    return unit;
}

extern "C" void djc_io_stop(void *u) {
    if (!u) return;
    auto unit = static_cast<AudioUnit>(u);
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}
#else
extern "C" int32_t djc_ioproc(void *, void *, const void *, uint32_t, uint32_t, void *) { return 0; }
extern "C" void *djc_io_start(djc_engine *, int32_t, int32_t *status) { if (status) *status = -1; return nullptr; }
extern "C" void djc_io_stop(void *) {}
#endif
