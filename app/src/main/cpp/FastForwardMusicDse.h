#ifndef MELONDS_FASTFORWARDMUSICDSE_H
#define MELONDS_FASTFORWARDMUSICDSE_H

#include "types.h"

namespace melonDS
{
    class NDS;
}

// Normal-speed music during fast-forward for games that use Procyon Studio's
// "Digital Sound Elements" (DSE) engine instead of Nintendo's SDAT format, such as
// Pokemon Mystery Dungeon: Explorers of Time/Darkness/Sky and Professor Layton 2-4.
namespace MelonDSAndroid::FastForwardMusic::Dse
{
    // Emulator thread
    void onRomLoaded(const melonDS::u8* rom, melonDS::u32 romLength) noexcept;
    bool isDseGame() noexcept;
    void onEmulatorFrame(melonDS::NDS& nds, bool fastForwardActive) noexcept;
    void disable() noexcept;
    void onReset() noexcept;
    void onStateLoaded() noexcept;
    void onShutdown() noexcept;
    void onSpuChannelStart(melonDS::u32 channelNum, melonDS::u32 srcAddr, melonDS::u32 loopPos, melonDS::u32 length) noexcept;

    // Any thread
    bool hasActiveSongForSpuFilter() noexcept;
    bool isBgmChannel(melonDS::u32 channelNum) noexcept;

    // Audio thread
    bool renderIfActive(melonDS::s16* audioData, int numFrames, bool mixWithExisting) noexcept;
    void advanceSilentFrames(int numFrames) noexcept;
}

#endif // MELONDS_FASTFORWARDMUSICDSE_H
