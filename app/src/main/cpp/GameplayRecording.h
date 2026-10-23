#ifndef MELONDS_GAMEPLAYRECORDING_H
#define MELONDS_GAMEPLAYRECORDING_H

#include <cstdint>

/**
 * Native helpers for the gameplay recorder. Game audio is tapped straight from the SPU mixer (see app/core-patches), so it
 * follows emulated time and isn't affected by fast-forward, the output volume or muting. It's resampled into a lock-free
 * ring buffer that the recorder drains. Captured frames are converted to the YUV 4:2:0 layout the video encoder expects.
 */
namespace MelonDSAndroid::GameplayRecording
{
    void setAudioCaptureEnabled(bool enabled) noexcept;
    // Called by the SPU on the emulator thread for every mixed sample, `cycles` SPU cycles after the previous one. Never
    // blocks; audio that doesn't fit in the ring buffer is dropped and counted
    void onSpuSample(std::int16_t left, std::int16_t right, std::uint32_t cycles) noexcept;
    std::int32_t readAudio(std::int16_t* interleavedStereo, std::int32_t maxFrames) noexcept;
    // Total frames dropped because the recorder fell behind, so it can keep its audio timestamps in sync
    std::uint64_t getDroppedAudioFrames() noexcept;
    std::int32_t getAudioSampleRate() noexcept;
    // Emulated time, advanced by the SPU. It stands still while the emulator is paused and runs faster while fast-forwarding
    std::int64_t getEmulatedTimeUs() noexcept;

    /**
     * Scales an ARGB frame with nearest-neighbour sampling and writes it as BT.601 limited-range YUV 4:2:0 into the given
     * planes. Strides follow android.media.Image.Plane, so both planar and semi-planar layouts work.
     */
    void convertArgbToYuv420(
        const std::uint32_t* source, int sourceWidth, int sourceHeight,
        int width, int height,
        std::uint8_t* yPlane, int yRowStride, int yPixelStride,
        std::uint8_t* uPlane, int uRowStride, int uPixelStride,
        std::uint8_t* vPlane, int vRowStride, int vPixelStride) noexcept;
}

#endif //MELONDS_GAMEPLAYRECORDING_H
