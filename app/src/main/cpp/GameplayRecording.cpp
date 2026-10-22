#include "GameplayRecording.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace MelonDSAndroid::GameplayRecording
{
    namespace
    {
        // Two seconds of 48 kHz stereo audio. The recorder drains it every few milliseconds
        constexpr std::uint64_t AudioCapacityFrames = 48000 * 2;

        std::int16_t audioBuffer[AudioCapacityFrames * 2];
        std::atomic<bool> audioCaptureEnabled{false};
        std::atomic<std::uint64_t> audioWriteFrames{0};
        std::atomic<std::uint64_t> audioReadFrames{0};
        std::atomic<std::int32_t> audioSampleRate{48000};

        inline std::uint8_t clampToByte(int value)
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
        }

        inline void splitArgb(std::uint32_t pixel, int& r, int& g, int& b)
        {
            r = static_cast<int>((pixel >> 16) & 0xFF);
            g = static_cast<int>((pixel >> 8) & 0xFF);
            b = static_cast<int>(pixel & 0xFF);
        }
    }

    void setAudioCaptureEnabled(bool enabled) noexcept
    {
        if (enabled)
        {
            // Called from the reader side, so discarding stale audio by moving the read position is safe
            audioReadFrames.store(audioWriteFrames.load(std::memory_order_acquire), std::memory_order_release);
        }
        audioCaptureEnabled.store(enabled, std::memory_order_release);
    }

    void writeAudio(const std::int16_t* interleavedStereo, std::int32_t frames) noexcept
    {
        if (frames <= 0 || !audioCaptureEnabled.load(std::memory_order_acquire))
            return;

        const std::uint64_t writePosition = audioWriteFrames.load(std::memory_order_relaxed);
        const std::uint64_t readPosition = audioReadFrames.load(std::memory_order_acquire);
        const std::uint64_t freeFrames = AudioCapacityFrames - std::min(writePosition - readPosition, AudioCapacityFrames);
        const std::uint64_t framesToWrite = std::min<std::uint64_t>(static_cast<std::uint64_t>(frames), freeFrames);
        if (framesToWrite == 0)
            return;

        const std::uint64_t start = writePosition % AudioCapacityFrames;
        const std::uint64_t firstPart = std::min(framesToWrite, AudioCapacityFrames - start);
        std::memcpy(&audioBuffer[start * 2], interleavedStereo, firstPart * 2 * sizeof(std::int16_t));
        if (firstPart < framesToWrite)
            std::memcpy(audioBuffer, &interleavedStereo[firstPart * 2], (framesToWrite - firstPart) * 2 * sizeof(std::int16_t));

        audioWriteFrames.store(writePosition + framesToWrite, std::memory_order_release);
    }

    std::int32_t readAudio(std::int16_t* interleavedStereo, std::int32_t maxFrames) noexcept
    {
        if (maxFrames <= 0)
            return 0;

        const std::uint64_t readPosition = audioReadFrames.load(std::memory_order_relaxed);
        const std::uint64_t writePosition = audioWriteFrames.load(std::memory_order_acquire);
        const std::uint64_t framesToRead = std::min<std::uint64_t>(writePosition - readPosition, static_cast<std::uint64_t>(maxFrames));
        if (framesToRead == 0)
            return 0;

        const std::uint64_t start = readPosition % AudioCapacityFrames;
        const std::uint64_t firstPart = std::min(framesToRead, AudioCapacityFrames - start);
        std::memcpy(interleavedStereo, &audioBuffer[start * 2], firstPart * 2 * sizeof(std::int16_t));
        if (firstPart < framesToRead)
            std::memcpy(&interleavedStereo[firstPart * 2], audioBuffer, (framesToRead - firstPart) * 2 * sizeof(std::int16_t));

        audioReadFrames.store(readPosition + framesToRead, std::memory_order_release);
        return static_cast<std::int32_t>(framesToRead);
    }

    void setAudioSampleRate(std::int32_t sampleRate) noexcept
    {
        if (sampleRate > 0)
            audioSampleRate.store(sampleRate, std::memory_order_relaxed);
    }

    std::int32_t getAudioSampleRate() noexcept
    {
        return audioSampleRate.load(std::memory_order_relaxed);
    }

    void convertArgbToYuv420(
        const std::uint32_t* source, int sourceWidth, int sourceHeight,
        int width, int height,
        std::uint8_t* yPlane, int yRowStride, int yPixelStride,
        std::uint8_t* uPlane, int uRowStride, int uPixelStride,
        std::uint8_t* vPlane, int vRowStride, int vPixelStride) noexcept
    {
        auto sourcePixel = [&](int x, int y) {
            const int sourceX = std::min(x * sourceWidth / width, sourceWidth - 1);
            const int sourceY = std::min(y * sourceHeight / height, sourceHeight - 1);
            return source[sourceY * sourceWidth + sourceX];
        };

        for (int y = 0; y < height; y++)
        {
            std::uint8_t* row = yPlane + y * yRowStride;
            for (int x = 0; x < width; x++)
            {
                int r, g, b;
                splitArgb(sourcePixel(x, y), r, g, b);
                row[x * yPixelStride] = clampToByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
        }

        // Chroma is subsampled 2x2. Averaging the block keeps colour edges clean at 1x, where each block covers 4 different pixels
        for (int y = 0; y < height / 2; y++)
        {
            std::uint8_t* uRow = uPlane + y * uRowStride;
            std::uint8_t* vRow = vPlane + y * vRowStride;
            for (int x = 0; x < width / 2; x++)
            {
                int rSum = 0, gSum = 0, bSum = 0;
                for (int dy = 0; dy < 2; dy++)
                {
                    for (int dx = 0; dx < 2; dx++)
                    {
                        int r, g, b;
                        splitArgb(sourcePixel(x * 2 + dx, y * 2 + dy), r, g, b);
                        rSum += r;
                        gSum += g;
                        bSum += b;
                    }
                }
                const int r = rSum / 4;
                const int g = gSum / 4;
                const int b = bSum / 4;
                uRow[x * uPixelStride] = clampToByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                vRow[x * vPixelStride] = clampToByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
}
