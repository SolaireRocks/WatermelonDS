#ifndef MELONDS_GAMEPLAYRECORDING_H
#define MELONDS_GAMEPLAYRECORDING_H

#include <cstdint>

/**
 * Native helpers for the gameplay recorder. Game audio is tapped from the audio output callback into a lock-free ring
 * buffer that the recorder drains, and captured frames are converted to the YUV 4:2:0 layout the video encoder expects.
 */
namespace MelonDSAndroid::GameplayRecording
{
    void setAudioCaptureEnabled(bool enabled) noexcept;
    // Called from the audio output callback. Never blocks; samples that don't fit are dropped
    void writeAudio(const std::int16_t* interleavedStereo, std::int32_t frames) noexcept;
    std::int32_t readAudio(std::int16_t* interleavedStereo, std::int32_t maxFrames) noexcept;
    void setAudioSampleRate(std::int32_t sampleRate) noexcept;
    std::int32_t getAudioSampleRate() noexcept;

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
