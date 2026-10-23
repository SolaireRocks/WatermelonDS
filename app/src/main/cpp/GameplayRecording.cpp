#include "GameplayRecording.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace MelonDSAndroid::GameplayRecording
{
    namespace
    {
        constexpr std::int32_t RecordingSampleRate = 48000;
        // Rate of the SPU cycles passed to onSpuSample (half the ARM7 clock)
        constexpr std::uint64_t SpuClockRate = 16756991;
        constexpr double SpuCyclesPerRecordingFrame = static_cast<double>(SpuClockRate) / RecordingSampleRate;
        // Four seconds of 48 kHz stereo audio. The recorder drains it every few milliseconds, but fast-forward fills it quicker
        constexpr std::uint64_t AudioCapacityFrames = RecordingSampleRate * 4;
        constexpr int StagingCapacityFrames = 256;

        std::int16_t audioBuffer[AudioCapacityFrames * 2];
        std::atomic<bool> audioCaptureEnabled{false};
        std::atomic<std::uint32_t> audioCaptureGeneration{0};
        std::atomic<std::uint64_t> audioWriteFrames{0};
        std::atomic<std::uint64_t> audioReadFrames{0};
        std::atomic<std::uint64_t> audioDroppedFrames{0};
        std::atomic<std::uint64_t> emulatedCycles{0};

        // Resampler state. Only touched by the emulator thread
        std::uint32_t producerGeneration = 0;
        double resamplePhase = 0.0;
        std::int16_t previousLeft = 0;
        std::int16_t previousRight = 0;
        std::int16_t staging[StagingCapacityFrames * 2];
        int stagingFrames = 0;

        void writeAudio(const std::int16_t* interleavedStereo, std::uint64_t frames) noexcept
        {
            const std::uint64_t writePosition = audioWriteFrames.load(std::memory_order_relaxed);
            const std::uint64_t readPosition = audioReadFrames.load(std::memory_order_acquire);
            const std::uint64_t freeFrames = AudioCapacityFrames - std::min(writePosition - readPosition, AudioCapacityFrames);
            const std::uint64_t framesToWrite = std::min(frames, freeFrames);
            if (framesToWrite < frames)
                audioDroppedFrames.fetch_add(frames - framesToWrite, std::memory_order_relaxed);
            if (framesToWrite == 0)
                return;

            const std::uint64_t start = writePosition % AudioCapacityFrames;
            const std::uint64_t firstPart = std::min(framesToWrite, AudioCapacityFrames - start);
            std::memcpy(&audioBuffer[start * 2], interleavedStereo, firstPart * 2 * sizeof(std::int16_t));
            if (firstPart < framesToWrite)
                std::memcpy(audioBuffer, &interleavedStereo[firstPart * 2], (framesToWrite - firstPart) * 2 * sizeof(std::int16_t));

            audioWriteFrames.store(writePosition + framesToWrite, std::memory_order_release);
        }

        inline std::int16_t interpolate(std::int16_t from, std::int16_t to, double t)
        {
            return static_cast<std::int16_t>(std::lround(from + (to - from) * t));
        }

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
            // Called from the reader side, so discarding stale audio by moving the read position is safe. The new generation
            // makes the emulator thread reset its resampler
            audioReadFrames.store(audioWriteFrames.load(std::memory_order_acquire), std::memory_order_release);
            audioCaptureGeneration.fetch_add(1, std::memory_order_release);
        }
        audioCaptureEnabled.store(enabled, std::memory_order_release);
    }

    void onSpuSample(std::int16_t left, std::int16_t right, std::uint32_t cycles) noexcept
    {
        emulatedCycles.store(emulatedCycles.load(std::memory_order_relaxed) + cycles, std::memory_order_release);
        if (!audioCaptureEnabled.load(std::memory_order_acquire))
            return;

        const std::uint32_t generation = audioCaptureGeneration.load(std::memory_order_acquire);
        if (generation != producerGeneration)
        {
            producerGeneration = generation;
            resamplePhase = 0.0;
            stagingFrames = 0;
            previousLeft = left;
            previousRight = right;
        }

        // Linear interpolation from the previous sample to this one, which comes `cycles` later
        while (resamplePhase < cycles)
        {
            const double t = resamplePhase / cycles;
            staging[stagingFrames * 2] = interpolate(previousLeft, left, t);
            staging[stagingFrames * 2 + 1] = interpolate(previousRight, right, t);
            if (++stagingFrames == StagingCapacityFrames)
            {
                writeAudio(staging, stagingFrames);
                stagingFrames = 0;
            }
            resamplePhase += SpuCyclesPerRecordingFrame;
        }
        resamplePhase -= cycles;
        previousLeft = left;
        previousRight = right;
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

    std::uint64_t getDroppedAudioFrames() noexcept
    {
        return audioDroppedFrames.load(std::memory_order_relaxed);
    }

    std::int32_t getAudioSampleRate() noexcept
    {
        return RecordingSampleRate;
    }

    std::int64_t getEmulatedTimeUs() noexcept
    {
        const std::uint64_t cycles = emulatedCycles.load(std::memory_order_acquire);
        return static_cast<std::int64_t>((cycles / SpuClockRate) * 1000000 + (cycles % SpuClockRate) * 1000000 / SpuClockRate);
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
