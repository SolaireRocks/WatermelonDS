#ifndef MELONDS_FASTFORWARDMUSIC_H
#define MELONDS_FASTFORWARDMUSIC_H

#include "types.h"

namespace melonDS
{
    class NDS;
    class SPUChannel;
}

namespace MelonDSAndroid::FastForwardMusic
{
    void setNormalSpeedMusicEnabled(bool enabled) noexcept;
    bool isNormalSpeedMusicEnabled() noexcept;

    void setSfxEnabled(bool enabled) noexcept;
    bool isSfxEnabled() noexcept;

    void onRomLoaded(const melonDS::u8* rom, melonDS::u32 romLength) noexcept;
    void onRomRead(melonDS::u32 addr, melonDS::u32 len) noexcept;
    void onEmulatorFrame(melonDS::NDS& nds) noexcept;
    void onReset() noexcept;
    void onStateLoaded(melonDS::NDS& nds) noexcept;
    void onShutdown() noexcept;

    void onSpuChannelStart(
        melonDS::u32 channelNum,
        melonDS::u32 cnt,
        melonDS::u32 srcAddr,
        melonDS::u16 timerReload,
        melonDS::u32 loopPos,
        melonDS::u32 length,
        const melonDS::u32* fifo) noexcept;

    void filterSpuSpeakerOutput(
        melonDS::u16 spuCnt,
        melonDS::u8 masterVolume,
        melonDS::u16 bias,
        bool applyBias,
        bool degrade10Bit,
        bool mute,
        melonDS::SPUChannel* channels,
        const melonDS::s32* channelSamples,
        melonDS::s16* output) noexcept;

    bool processAudioOutput(
        melonDS::s16* audioData,
        int numFrames,
        bool fastForwardActive,
        bool muteOnFastForward) noexcept;
}

#endif // MELONDS_FASTFORWARDMUSIC_H
