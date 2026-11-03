#include "FastForwardMusic.h"
#include "FastForwardMusicDse.h"
#include "MelonDS.h"
#include "NDS.h"
#include "SPU.h"
#include "Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace MelonDSAndroid::FastForwardMusic
{
namespace
{
    using melonDS::s8;
    using melonDS::u8;
    using melonDS::s16;
    using melonDS::u16;
    using melonDS::s32;
    using melonDS::u32;
    using melonDS::s64;
    using melonDS::u64;

    constexpr double kOutputSampleRate = 48000.0;
    constexpr double kSpuClockHz = 16756991.0;
    constexpr double kSeqTimerHz = 33513982.0 / (64.0 * 2728.0); // ~192.000355 Hz
    constexpr double kOutputFramesPerSeqTick = kOutputSampleRate / kSeqTimerHz;
    constexpr int kMaxVoices = 24;
    constexpr int kMaxTracks = 16;

    // Magic constants (little-endian)
    constexpr u32 kMagicSDAT = 0x54414453u; // "SDAT"
    constexpr u32 kMagicINFO = 0x4F464E49u; // "INFO"
    constexpr u32 kMagicFAT  = 0x20544146u; // "FAT "
    constexpr u32 kMagicSSEQ = 0x51455353u; // "SSEQ"
    constexpr u32 kMagicSSAR = 0x52415353u; // "SSAR"
    constexpr u32 kMagicSBNK = 0x4B4E4253u; // "SBNK"
    constexpr u32 kMagicSWAR = 0x52415753u; // "SWAR"
    constexpr u32 kMagicSTRM = 0x4D525453u; // "STRM"
    constexpr u32 kMagicDATA = 0x41544144u; // "DATA"
    constexpr u32 kMagicHEAD = 0x44414548u; // "HEAD"

    inline u8 readU8(const u8* ptr) noexcept
    {
        return ptr[0];
    }

    inline s8 readS8(const u8* ptr) noexcept
    {
        return static_cast<s8>(ptr[0]);
    }

    inline u16 readU16(const u8* ptr) noexcept
    {
        return static_cast<u16>(ptr[0]) | (static_cast<u16>(ptr[1]) << 8);
    }

    inline s16 readS16(const u8* ptr) noexcept
    {
        return static_cast<s16>(readU16(ptr));
    }

    inline u32 readU24(const u8* ptr) noexcept
    {
        return static_cast<u32>(ptr[0])
            | (static_cast<u32>(ptr[1]) << 8)
            | (static_cast<u32>(ptr[2]) << 16);
    }

    inline u32 readU32(const u8* ptr) noexcept
    {
        return static_cast<u32>(ptr[0])
            | (static_cast<u32>(ptr[1]) << 8)
            | (static_cast<u32>(ptr[2]) << 16)
            | (static_cast<u32>(ptr[3]) << 24);
    }

    u64 fnv1a64(const u8* data, size_t len) noexcept
    {
        u64 hash = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i)
        {
            hash ^= static_cast<u64>(data[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    u64 mixWords64(u32 a, u32 b, u32 c, u32 d) noexcept
    {
        u64 h = 14695981039346656037ULL;
        auto mix = [&](u32 v) {
            h ^= v;
            h *= 1099511628211ULL;
        };
        mix(a);
        mix(b);
        mix(c);
        mix(d);
        return h;
    }

    // Hardware SPU sample signature for matching BGM samples vs SFX samples
    struct SampleSignature
    {
        u8 waveType = 0;
        u32 loopPosBytes = 0;
        u32 lengthBytes = 0;
        u32 fifo0 = 0;
        u32 fifo1 = 0;

        bool matches(u8 type, u32 loopPos, u32 length, u32 f0, u32 f1) const noexcept
        {
            return waveType == type
                && loopPosBytes == loopPos
                && lengthBytes == length
                && fifo0 == f0
                && fifo1 == f1;
        }
    };

    struct DecodedSwav
    {
        std::vector<s16> pcm;
        bool loop = false;
        u32 loopStartSample = 0;
        double baseSampleRate = 16000.0;
        SampleSignature signature {};
    };

    struct SubInstrument
    {
        u8 type = 0; // 0=none, 1=SWAV, 2=PSG, 3=Noise
        u16 swavNo = 0;
        u16 swarNo = 0;
        u8 origKey = 60;
        u8 attack = 127;
        u8 decay = 127;
        u8 sustain = 127;
        u8 release = 127;
        u8 pan = 64;
        s32 decodedSampleIndex = -1;
    };

    struct SbnkInstrument
    {
        u8 recordType = 0; // 0=empty, 1..4=single, 16=drumset, 17=keysplit
        SubInstrument single {};
        u8 minKey = 0;
        u8 maxKey = 0;
        std::array<u8, 8> keySplitBounds {};
        std::vector<SubInstrument> regions;

        const SubInstrument* resolveForKey(u8 key) const noexcept
        {
            if (recordType >= 1 && recordType <= 4)
                return &single;
            if (recordType == 16)
            {
                if (key < minKey || key > maxKey)
                    return nullptr;
                const size_t idx = static_cast<size_t>(key - minKey);
                if (idx >= regions.size() || regions[idx].type == 0)
                    return nullptr;
                return &regions[idx];
            }
            if (recordType == 17)
            {
                for (size_t r = 0; r < 8 && r < regions.size(); ++r)
                {
                    if (keySplitBounds[r] != 0 && key <= keySplitBounds[r])
                    {
                        return regions[r].type != 0 ? &regions[r] : nullptr;
                    }
                }
            }
            return nullptr;
        }
    };

    struct PreparedStrm
    {
        std::vector<u8> strmData;
        u8 waveType = 0;
        bool loop = false;
        u8 numChannels = 1;
        u16 sampleRate = 32768;
        u32 loopStartSample = 0;
        u32 numSamples = 0;
        u32 dataOffset = 0x68;
        u32 numBlocks = 0;
        u32 blockSize = 0;
        u32 samplesPerBlock = 0;
        u32 lastBlockSize = 0;
        u32 lastBlockSamples = 0;
        u8 volume = 127;
    };

    struct PreparedSong
    {
        u64 songKey = 0;
        bool isStream = false;

        // SSEQ song data
        std::vector<u8> sseqData;
        u8 seqVolume = 127;
        bool hasLoopJump = false;
        u32 minLoopTargetOffset = 0xFFFFFFFFu;
        std::vector<SbnkInstrument> instruments;
        std::vector<DecodedSwav> samples;
        std::vector<SampleSignature> bgmSignatures;
        u8 bgmPsgDutyMask = 0;
        bool bgmUsesNoise = false;

        // STRM song data
        std::unique_ptr<PreparedStrm> strm;
    };

    // Static CFG analysis of SSEQ bytecode to find loop behavior and used programs
    void analyzeSseqBytecode(
        const u8* data,
        u32 size,
        bool& outHasLoopJump,
        u32& outMinLoopTarget,
        std::vector<bool>& outUsedPrograms)
    {
        outHasLoopJump = false;
        outMinLoopTarget = 0xFFFFFFFFu;
        outUsedPrograms.assign(256, false);
        outUsedPrograms[0] = true;

        if (!data || size == 0)
            return;

        std::vector<bool> visited(size, false);
        std::vector<u32> worklist;
        worklist.reserve(32);
        worklist.push_back(0);

        auto readVlqAt = [&](u32& pc) -> u32 {
            u32 value = 0;
            for (int i = 0; i < 4 && pc < size; ++i)
            {
                const u8 b = data[pc++];
                value = (value << 7) | (b & 0x7Fu);
                if ((b & 0x80u) == 0)
                    break;
            }
            return value;
        };

        while (!worklist.empty())
        {
            u32 pc = worklist.back();
            worklist.pop_back();

            bool conditionalNext = false;
            while (pc < size)
            {
                if (visited[pc])
                    break;
                visited[pc] = true;

                const u32 instrStart = pc;
                u8 op = data[pc++];

                bool wasConditional = conditionalNext;
                conditionalNext = false;

                if (op == 0xA2) // If prefix
                {
                    conditionalNext = true;
                    continue;
                }

                if (op == 0xA0) // Random prefix: subOpcode + args + s16 min + s16 max
                {
                    if (pc >= size)
                        break;
                    const u8 sub = data[pc++];
                    if (sub < 0x80)
                        pc += 1 + 4; // vel + min + max
                    else if (sub >= 0xB0 && sub <= 0xBD)
                        pc += 1 + 4; // varNo + min + max
                    else
                        pc += 4; // min + max
                    continue;
                }

                if (op == 0xA1) // FromVar prefix: subOpcode + args + u8 varNo
                {
                    if (pc >= size)
                        break;
                    const u8 sub = data[pc++];
                    if (sub < 0x80)
                        pc += 1 + 1; // vel + varNo
                    else if (sub >= 0xB0 && sub <= 0xBD)
                        pc += 1 + 1; // dstVar + srcVar
                    else
                        pc += 1; // varNo
                    continue;
                }

                if (op < 0x80) // Note-On: vel (u8) + duration (VLQ)
                {
                    if (pc >= size)
                        break;
                    pc++; // velocity
                    (void)readVlqAt(pc);
                }
                else if (op == 0x80) // Rest: VLQ
                {
                    (void)readVlqAt(pc);
                }
                else if (op == 0x81) // Program change: VLQ
                {
                    const u32 prog = readVlqAt(pc) & 0x7FFFu;
                    if (prog >= outUsedPrograms.size())
                        outUsedPrograms.resize(std::min<u32>(prog + 1, 2048), false);
                    if (prog < outUsedPrograms.size())
                        outUsedPrograms[prog] = true;
                }
                else if (op == 0x93) // OpenTrack: u8 trk + u24 addr
                {
                    if (pc + 4 > size)
                        break;
                    pc++; // track no
                    const u32 target = readU24(data + pc);
                    pc += 3;
                    if (target < size && !visited[target])
                        worklist.push_back(target);
                }
                else if (op == 0x94) // Jump: u24 addr
                {
                    if (pc + 3 > size)
                        break;
                    const u32 target = readU24(data + pc);
                    pc += 3;
                    outHasLoopJump = true;
                    if (target <= instrStart)
                        outMinLoopTarget = std::min(outMinLoopTarget, target);
                    if (target < size && !visited[target])
                        worklist.push_back(target);
                    if (!wasConditional)
                        break;
                }
                else if (op == 0x95) // Call: u24 addr
                {
                    if (pc + 3 > size)
                        break;
                    const u32 target = readU24(data + pc);
                    pc += 3;
                    if (target < size && !visited[target])
                        worklist.push_back(target);
                }
                else if (op >= 0xB0 && op <= 0xBD) // Variable op: u8 var + s16 val
                {
                    pc += 3;
                }
                else if ((op >= 0xC0 && op <= 0xD6)) // 1-byte arg commands
                {
                    if (op == 0xD4 && pc < size && data[pc] == 0)
                        outHasLoopJump = true; // infinite loop start
                    pc += 1;
                }
                else if (op == 0xE0 || op == 0xE1 || op == 0xE3) // 2-byte arg commands
                {
                    pc += 2;
                }
                else if (op == 0xFE) // Allocate tracks: u16 mask
                {
                    pc += 2;
                }
                else if (op == 0xFC) // Loop end: 0 args
                {
                }
                else if (op == 0xFD || op == 0xFF) // Return / Fin
                {
                    if (!wasConditional)
                        break;
                }
                else
                {
                    // Unknown opcode, stop walking this branch safely
                    break;
                }
            }
        }
    }

    bool decodeSwavFromSwar(
        const u8* swarBase,
        u32 swarSize,
        u16 swavNo,
        DecodedSwav& outSwav)
    {
        if (!swarBase || swarSize < 0x40)
            return false;
        if (readU32(swarBase) != kMagicSWAR || readU32(swarBase + 0x10) != kMagicDATA)
            return false;

        const u32 numSamples = readU32(swarBase + 0x38);
        if (swavNo >= numSamples || 0x3Cu + (static_cast<u32>(swavNo) + 1u) * 4u > swarSize)
            return false;

        const u32 sampleOffset = readU32(swarBase + 0x3C + static_cast<u32>(swavNo) * 4u);
        if (sampleOffset < 0x3C || sampleOffset + 0x0C > swarSize)
            return false;

        const u8* swavPtr = swarBase + sampleOffset;
        const u8 waveType = readU8(swavPtr + 0x00);
        const u8 loopFlag = readU8(swavPtr + 0x01);
        const u16 sampleRate = readU16(swavPtr + 0x02);
        const u16 timerReload = readU16(swavPtr + 0x04);
        const u16 loopOffsetWords = readU16(swavPtr + 0x06);
        const u32 nonLoopWords = readU32(swavPtr + 0x08) & 0x001FFFFFu;

        const u32 loopBytes = static_cast<u32>(loopOffsetWords) << 2;
        const u32 nonLoopBytes = nonLoopWords << 2;
        const u64 totalBytes64 = static_cast<u64>(loopBytes) + static_cast<u64>(nonLoopBytes);
        if (totalBytes64 == 0 || sampleOffset + 0x0Cu + totalBytes64 > swarSize)
            return false;

        const u32 totalBytes = static_cast<u32>(totalBytes64);
        const u8* rawData = swavPtr + 0x0C;

        outSwav.loop = (loopFlag != 0);
        outSwav.baseSampleRate = (timerReload > 0)
            ? (kSpuClockHz / static_cast<double>(timerReload))
            : (sampleRate > 0 ? static_cast<double>(sampleRate) : 16000.0);

        outSwav.signature.waveType = waveType;
        outSwav.signature.loopPosBytes = loopBytes;
        outSwav.signature.lengthBytes = nonLoopBytes;
        outSwav.signature.fifo0 = totalBytes >= 4 ? readU32(rawData) : 0;
        outSwav.signature.fifo1 = totalBytes >= 8 ? readU32(rawData + 4) : 0;

        if (waveType == 0) // PCM8
        {
            outSwav.pcm.resize(totalBytes);
            for (u32 i = 0; i < totalBytes; ++i)
                outSwav.pcm[i] = static_cast<s16>(static_cast<s8>(rawData[i]) * 256);
            outSwav.loopStartSample = std::min(loopBytes, totalBytes);
            return true;
        }
        else if (waveType == 1) // PCM16
        {
            const u32 numPcmSamples = totalBytes >> 1;
            if (numPcmSamples == 0)
                return false;
            outSwav.pcm.resize(numPcmSamples);
            for (u32 i = 0; i < numPcmSamples; ++i)
                outSwav.pcm[i] = readS16(rawData + i * 2);
            outSwav.loopStartSample = std::min(loopBytes >> 1, numPcmSamples);
            return true;
        }
        else if (waveType == 2) // IMA-ADPCM
        {
            if (totalBytes <= 4)
                return false;
            const u32 adpcmBytes = totalBytes - 4;
            const u32 numNibbles = adpcmBytes * 2;
            outSwav.pcm.resize(numNibbles);

            s32 val = static_cast<s32>(readS16(rawData));
            s32 idx = std::clamp(static_cast<int>(readU16(rawData + 2) & 0x7Fu), 0, 88);

            for (u32 b = 0; b < adpcmBytes; ++b)
            {
                const u8 byteVal = rawData[4 + b];
                for (int n = 0; n < 2; ++n)
                {
                    const u8 nib = (n == 0) ? (byteVal & 0x0Fu) : (byteVal >> 4);
                    const u16 step = melonDS::SPUChannel::ADPCMTable[idx];
                    u16 diff = step >> 3;
                    if (nib & 1) diff += (step >> 2);
                    if (nib & 2) diff += (step >> 1);
                    if (nib & 4) diff += step;

                    if (nib & 8)
                    {
                        val -= diff;
                        if (val < -0x7FFF) val = -0x7FFF;
                    }
                    else
                    {
                        val += diff;
                        if (val > 0x7FFF) val = 0x7FFF;
                    }

                    idx += melonDS::SPUChannel::ADPCMIndexTable[nib & 7];
                    if (idx < 0) idx = 0;
                    else if (idx > 88) idx = 88;

                    outSwav.pcm[b * 2 + n] = static_cast<s16>(val);
                }
            }

            const u32 loopNibble = (loopBytes >= 4) ? ((loopBytes - 4) * 2) : 0;
            outSwav.loopStartSample = std::min(loopNibble, numNibbles);
            return true;
        }

        return false;
    }

    std::shared_ptr<PreparedSong> buildPreparedSseqSong(
        u64 songKey,
        const u8* sseqData,
        u32 sseqDataSize,
        u8 seqVolume,
        const u8* sbnkFile,
        u32 sbnkFileSize,
        const std::array<const u8*, 4>& swarFiles,
        const std::array<u32, 4>& swarFileSizes)
    {
        if (!sseqData || sseqDataSize == 0 || !sbnkFile || sbnkFileSize < 0x3C)
            return nullptr;
        if (readU32(sbnkFile) != kMagicSBNK || readU32(sbnkFile + 0x10) != kMagicDATA)
            return nullptr;

        auto song = std::make_shared<PreparedSong>();
        song->songKey = songKey;
        song->isStream = false;
        song->seqVolume = (seqVolume > 0 && seqVolume <= 127) ? seqVolume : 127;
        song->sseqData.assign(sseqData, sseqData + sseqDataSize);

        std::vector<bool> usedPrograms;
        analyzeSseqBytecode(
            song->sseqData.data(),
            static_cast<u32>(song->sseqData.size()),
            song->hasLoopJump,
            song->minLoopTargetOffset,
            usedPrograms);

        const u32 numInstruments = std::min<u32>(readU32(sbnkFile + 0x38), 2048u);
        song->instruments.resize(numInstruments);

        // Map (swarNo, swavNo) -> index in song->samples
        struct SampleKey
        {
            u16 swarNo;
            u16 swavNo;
            bool operator==(const SampleKey& o) const noexcept
            {
                return swarNo == o.swarNo && swavNo == o.swavNo;
            }
        };
        std::vector<SampleKey> sampleKeys;

        auto getOrDecodeSample = [&](u16 swarNo, u16 swavNo) -> s32 {
            if (swarNo >= 4 || !swarFiles[swarNo] || swarFileSizes[swarNo] < 0x40)
                return -1;
            for (size_t i = 0; i < sampleKeys.size(); ++i)
            {
                if (sampleKeys[i].swarNo == swarNo && sampleKeys[i].swavNo == swavNo)
                    return static_cast<s32>(i);
            }
            DecodedSwav decoded;
            if (!decodeSwavFromSwar(swarFiles[swarNo], swarFileSizes[swarNo], swavNo, decoded))
                return -1;
            const s32 index = static_cast<s32>(song->samples.size());
            sampleKeys.push_back({swarNo, swavNo});
            song->samples.push_back(std::move(decoded));
            return index;
        };

        auto parseSubInstrument = [&](u8 type, const u8* paramPtr, bool isProgramUsed) -> SubInstrument {
            SubInstrument sub {};
            sub.type = type;
            sub.swavNo = readU16(paramPtr + 0);
            sub.swarNo = readU16(paramPtr + 2);
            sub.origKey = readU8(paramPtr + 4);
            sub.attack = readU8(paramPtr + 5);
            sub.decay = readU8(paramPtr + 6);
            sub.sustain = readU8(paramPtr + 7);
            sub.release = readU8(paramPtr + 8);
            sub.pan = readU8(paramPtr + 9);
            if (type == 1)
            {
                sub.decodedSampleIndex = getOrDecodeSample(sub.swarNo, sub.swavNo);
                if (sub.decodedSampleIndex >= 0 && isProgramUsed)
                {
                    const auto& sig = song->samples[sub.decodedSampleIndex].signature;
                    bool exists = false;
                    for (const auto& existing : song->bgmSignatures)
                    {
                        if (existing.matches(sig.waveType, sig.loopPosBytes, sig.lengthBytes, sig.fifo0, sig.fifo1))
                        {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists)
                        song->bgmSignatures.push_back(sig);
                }
            }
            else if (type == 2 && isProgramUsed)
            {
                song->bgmPsgDutyMask |= static_cast<u8>(1u << (sub.swavNo & 7u));
            }
            else if (type == 3 && isProgramUsed)
            {
                song->bgmUsesNoise = true;
            }
            return sub;
        };

        for (u32 i = 0; i < numInstruments; ++i)
        {
            if (0x3Cu + (i + 1u) * 4u > sbnkFileSize)
                break;
            const u32 record = readU32(sbnkFile + 0x3C + i * 4u);
            const u8 fRecord = static_cast<u8>(record & 0xFFu);
            const u32 offset = (record >> 8) & 0xFFFFFFu;
            if (fRecord == 0 || offset < 0x3C || offset >= sbnkFileSize)
                continue;

            const bool isProgramUsed = (i < usedPrograms.size() && usedPrograms[i]);
            SbnkInstrument& inst = song->instruments[i];
            inst.recordType = fRecord;

            if (fRecord >= 1 && fRecord <= 4)
            {
                if (offset + 10 > sbnkFileSize)
                    continue;
                inst.single = parseSubInstrument(fRecord, sbnkFile + offset, isProgramUsed);
            }
            else if (fRecord == 16) // Drum set
            {
                if (offset + 2 > sbnkFileSize)
                    continue;
                inst.minKey = readU8(sbnkFile + offset + 0);
                inst.maxKey = readU8(sbnkFile + offset + 1);
                if (inst.maxKey < inst.minKey)
                    continue;
                const u32 count = static_cast<u32>(inst.maxKey - inst.minKey) + 1u;
                inst.regions.resize(count);
                for (u32 k = 0; k < count; ++k)
                {
                    const u32 subOff = offset + 2u + k * 12u;
                    if (subOff + 12 > sbnkFileSize)
                        break;
                    const u8 subType = readU8(sbnkFile + subOff);
                    if (subType >= 1 && subType <= 4)
                        inst.regions[k] = parseSubInstrument(subType, sbnkFile + subOff + 2, isProgramUsed);
                }
            }
            else if (fRecord == 17) // Key split
            {
                if (offset + 8 > sbnkFileSize)
                    continue;
                u32 numRegions = 0;
                for (u32 r = 0; r < 8; ++r)
                {
                    inst.keySplitBounds[r] = readU8(sbnkFile + offset + r);
                    if (inst.keySplitBounds[r] != 0)
                        numRegions = r + 1;
                }
                inst.regions.resize(numRegions);
                for (u32 r = 0; r < numRegions; ++r)
                {
                    const u32 subOff = offset + 8u + r * 12u;
                    if (subOff + 12 > sbnkFileSize)
                        break;
                    const u8 subType = readU8(sbnkFile + subOff);
                    if (subType >= 1 && subType <= 4)
                        inst.regions[r] = parseSubInstrument(subType, sbnkFile + subOff + 2, isProgramUsed);
                }
            }
        }

        // If the SSEQ uses indirect FromVar program changes so no static 0x81 was seen, include all SBNK samples in bgmSignatures
        if (song->bgmSignatures.empty() && !song->samples.empty())
        {
            for (const auto& swav : song->samples)
                song->bgmSignatures.push_back(swav.signature);
        }

        return song;
    }

    std::shared_ptr<PreparedSong> buildPreparedStrmSong(
        u64 songKey,
        const u8* strmFile,
        u32 strmFileSize,
        u8 strmVolume)
    {
        if (!strmFile || strmFileSize < 0x68)
            return nullptr;
        if (readU32(strmFile) != kMagicSTRM || readU32(strmFile + 0x10) != kMagicHEAD)
            return nullptr;

        auto strm = std::make_unique<PreparedStrm>();
        strm->waveType = readU8(strmFile + 0x18);
        strm->loop = readU8(strmFile + 0x19) != 0;
        strm->numChannels = std::clamp<u8>(readU8(strmFile + 0x1A), 1, 2);
        strm->sampleRate = readU16(strmFile + 0x1C);
        strm->loopStartSample = readU32(strmFile + 0x20);
        strm->numSamples = readU32(strmFile + 0x24);
        strm->dataOffset = readU32(strmFile + 0x28);
        strm->numBlocks = readU32(strmFile + 0x2C);
        strm->blockSize = readU32(strmFile + 0x30);
        strm->samplesPerBlock = readU32(strmFile + 0x34);
        strm->lastBlockSize = readU32(strmFile + 0x38);
        strm->lastBlockSamples = readU32(strmFile + 0x3C);
        strm->volume = (strmVolume > 0 && strmVolume <= 127) ? strmVolume : 127;

        if (strm->numBlocks == 0 || strm->samplesPerBlock == 0 || strm->dataOffset >= strmFileSize)
            return nullptr;

        strm->strmData.assign(strmFile, strmFile + strmFileSize);

        auto song = std::make_shared<PreparedSong>();
        song->songKey = songKey;
        song->isStream = true;
        song->hasLoopJump = strm->loop;
        song->strm = std::move(strm);
        return song;
    }

    // =========================================================================
    // Custom Nitro SSEQ + STRM Synthesizer Engine
    // =========================================================================

    struct SeqTrackState
    {
        bool active = false;
        bool noteWait = false;
        bool tie = false;
        bool portaEnabled = false;
        bool cmpFlag = true;

        u32 cur = 0;
        s32 wait = 0;

        u16 program = 0;
        u8 volume = 127;
        u8 expression = 127;
        u8 pan = 64;
        s8 pitchBend = 0;
        u8 pitchBendRange = 2;
        s8 transpose = 0;

        u8 attack = 0xFF;
        u8 decay = 0xFF;
        u8 sustain = 0xFF;
        u8 release = 0xFF;

        u8 portaKey = 60;
        u8 portaTime = 0;
        s16 sweepPitch = 0;

        u8 modType = 0;
        u8 modDepth = 0;
        u8 modSpeed = 16;
        u8 modRange = 1;
        u16 modDelay = 0;

        std::array<u32, 3> callStack {};
        std::array<u8, 3> loopCount {};
        u8 callStackDepth = 0;

        void init(u32 startOffset) noexcept
        {
            *this = SeqTrackState{};
            active = true;
            cur = startOffset;
        }
    };

    enum class EnvPhase : u8
    {
        Idle = 0,
        Attack,
        Decay,
        Sustain,
        Release
    };

    struct SynthVoice
    {
        bool active = false;
        u8 trackIndex = 0;
        u8 noteKey = 60;
        u8 velocity = 127;
        s32 noteTicksRemaining = 0; // <0 for tied/infinite until note-off

        u8 instType = 0; // 1=SWAV, 2=PSG, 3=Noise
        s32 sampleIndex = -1;
        u8 psgDuty = 0;
        u16 noiseLfsr = 0x7FFF;

        u8 origKey = 60;
        u8 instPan = 64;

        EnvPhase envPhase = EnvPhase::Idle;
        s32 envLevel = -92544; // [-92544, 0] in 1/1024 dB
        u8 attackCoeff = 0;
        s32 decayRate = 0xFFFF;
        s32 sustainLevel = 0;
        s32 releaseRate = 0xFFFF;
        float currentLinearGain = 0.0f;

        double samplePhase = 0.0;
        double sampleStep = 0.0;

        // Sweep / Portamento & LFO
        s32 sweepCurrent = 0;
        s32 sweepTarget = 0;
        s32 sweepLen = 0;
        s32 sweepCnt = 0;
        u16 modDelayCnt = 0;
        u16 lfoPhase = 0;

        u64 birthOrder = 0;
    };

    u8 calcAttackCoeff(u8 attack) noexcept
    {
        const int a = std::clamp<int>(attack, 0, 127);
        if (a == 127)
            return 0;
        if (a >= 109)
            return static_cast<u8>((127 - a) * 167 / 19);
        return static_cast<u8>(255 - (a * 88 / 109));
    }

    s32 calcDecayRate(u8 rate) noexcept
    {
        const int r = std::clamp<int>(rate, 0, 127);
        if (r == 127)
            return 0xFFFF;
        if (r == 126)
            return 0x3C00;
        if (r >= 50)
            return 0x3C00 / (127 - r);
        return r * 4 + 1;
    }

    s32 calcSustainDb(u8 sustain) noexcept
    {
        const int s = std::clamp<int>(sustain, 0, 127);
        if (s == 127)
            return 0;
        if (s == 0)
            return -92544;
        const double db = 20.0 * std::log10(static_cast<double>(s) / 127.0);
        return std::clamp<s32>(static_cast<s32>(db * 1024.0), -92544, 0);
    }

    class CustomMusicSynthesizer
    {
    public:
        void setSong(std::shared_ptr<const PreparedSong> newSong, u32 initialFastForwardTicks192 = 0) noexcept
        {
            if (!newSong)
            {
                if (currentSong)
                {
                    fadeOutRemaining = kFadeSamples;
                }
                return;
            }

            if (currentSong && currentSong->songKey == newSong->songKey && !fadingOutToSilence)
                return;

            currentSong = std::move(newSong);
            fadingOutToSilence = false;
            fadeOutRemaining = 0;
            fadeInRemaining = kFadeSamples;
            resetSongState();

            if (!currentSong->isStream && initialFastForwardTicks192 > 0)
            {
                const u32 cappedTicks = std::min<u32>(initialFastForwardTicks192, 192u * 45u);
                for (u32 t = 0; t < cappedTicks; ++t)
                    stepSequencerOneTimerTick();
            }
        }

        void stopSong() noexcept
        {
            if (currentSong && !fadingOutToSilence)
            {
                fadingOutToSilence = true;
                fadeOutRemaining = kFadeSamples;
            }
        }

        void clearImmediate() noexcept
        {
            currentSong.reset();
            fadingOutToSilence = false;
            fadeOutRemaining = 0;
            fadeInRemaining = 0;
            for (auto& v : voices)
                v.active = false;
        }

        bool hasActiveSong() const noexcept
        {
            return currentSong != nullptr && !fadingOutToSilence;
        }

        u64 currentSongKey() const noexcept
        {
            return (currentSong && !fadingOutToSilence) ? currentSong->songKey : 0;
        }

        // Advance sequencer at 1x speed while fast-forward is off so song position stays synced
        void advanceSilentFrames(int numFrames) noexcept
        {
            if (!currentSong || fadingOutToSilence || numFrames <= 0)
                return;

            if (currentSong->isStream)
            {
                if (currentSong->strm)
                {
                    const double strmStep = static_cast<double>(currentSong->strm->sampleRate) / kOutputSampleRate;
                    strmSamplePos += strmStep * numFrames;
                    const double total = static_cast<double>(currentSong->strm->numSamples);
                    if (strmSamplePos >= total && total > 0.0)
                    {
                        if (currentSong->strm->loop)
                        {
                            const double loopStart = static_cast<double>(currentSong->strm->loopStartSample);
                            const double loopLen = std::max(1.0, total - loopStart);
                            strmSamplePos = loopStart + std::fmod(strmSamplePos - loopStart, loopLen);
                        }
                        else
                        {
                            strmSamplePos = total;
                        }
                    }
                }
                return;
            }

            tickSamplePhase += static_cast<double>(numFrames);
            while (tickSamplePhase >= kOutputFramesPerSeqTick)
            {
                tickSamplePhase -= kOutputFramesPerSeqTick;
                stepSequencerOneTimerTick();
            }
        }

        void renderAndMix(s16* audioData, int numFrames, bool mixWithExisting) noexcept
        {
            if (!currentSong || numFrames <= 0)
            {
                if (!mixWithExisting && numFrames > 0)
                    std::fill_n(audioData, numFrames * 2, 0);
                return;
            }

            if (currentSong->isStream)
            {
                renderStream(audioData, numFrames, mixWithExisting);
                return;
            }

            for (int frame = 0; frame < numFrames; ++frame)
            {
                tickSamplePhase += 1.0;
                while (tickSamplePhase >= kOutputFramesPerSeqTick)
                {
                    tickSamplePhase -= kOutputFramesPerSeqTick;
                    stepSequencerOneTimerTick();
                }

                float mixLeft = 0.0f;
                float mixRight = 0.0f;

                for (SynthVoice& v : voices)
                {
                    if (!v.active || v.currentLinearGain <= 0.00005f)
                        continue;

                    float sampleVal = 0.0f;
                    if (v.instType == 1) // SWAV
                    {
                        if (v.sampleIndex < 0 || static_cast<size_t>(v.sampleIndex) >= currentSong->samples.size())
                        {
                            v.active = false;
                            continue;
                        }
                        const DecodedSwav& swav = currentSong->samples[v.sampleIndex];
                        const u32 total = static_cast<u32>(swav.pcm.size());
                        if (total == 0)
                        {
                            v.active = false;
                            continue;
                        }
                        if (v.samplePhase >= static_cast<double>(total))
                        {
                            if (swav.loop && swav.loopStartSample < total)
                            {
                                const double loopLen = static_cast<double>(total - swav.loopStartSample);
                                v.samplePhase = static_cast<double>(swav.loopStartSample)
                                    + std::fmod(v.samplePhase - static_cast<double>(swav.loopStartSample), loopLen);
                            }
                            else
                            {
                                v.active = false;
                                continue;
                            }
                        }

                        const u32 idx0 = static_cast<u32>(v.samplePhase);
                        const double frac = v.samplePhase - static_cast<double>(idx0);
                        u32 idx1 = idx0 + 1;
                        if (idx1 >= total)
                            idx1 = (swav.loop && swav.loopStartSample < total) ? swav.loopStartSample : idx0;

                        const float s0 = static_cast<float>(swav.pcm[idx0]);
                        const float s1 = static_cast<float>(swav.pcm[idx1]);
                        sampleVal = s0 + (s1 - s0) * static_cast<float>(frac);
                        v.samplePhase += v.sampleStep;
                    }
                    else if (v.instType == 2) // PSG Square Wave
                    {
                        const u32 stepIdx = static_cast<u32>(v.samplePhase) & 7u;
                        sampleVal = static_cast<float>(melonDS::SPUChannel::PSGTable[v.psgDuty & 7u][stepIdx]);
                        v.samplePhase += v.sampleStep;
                        if (v.samplePhase >= 8.0)
                            v.samplePhase -= 8.0 * std::floor(v.samplePhase / 8.0);
                    }
                    else if (v.instType == 3) // PSG Noise
                    {
                        while (v.samplePhase >= 1.0)
                        {
                            v.samplePhase -= 1.0;
                            if (v.noiseLfsr & 1u)
                                v.noiseLfsr = (v.noiseLfsr >> 1) ^ 0x6000u;
                            else
                                v.noiseLfsr >>= 1;
                        }
                        sampleVal = (v.noiseLfsr & 1u) ? -16384.0f : 16384.0f;
                        v.samplePhase += v.sampleStep;
                    }

                    const SeqTrackState& trk = tracks[v.trackIndex];
                    const int pan = std::clamp<int>(static_cast<int>(v.instPan) + static_cast<int>(trk.pan) - 64, 0, 128);
                    const float panLeft = static_cast<float>(128 - pan) * (1.0f / 128.0f);
                    const float panRight = static_cast<float>(pan) * (1.0f / 128.0f);

                    mixLeft += sampleVal * v.currentLinearGain * panLeft;
                    mixRight += sampleVal * v.currentLinearGain * panRight;
                }

                float fadeScale = 0.42f;
                if (fadeInRemaining > 0)
                {
                    fadeScale *= static_cast<float>(kFadeSamples - fadeInRemaining) / static_cast<float>(kFadeSamples);
                    --fadeInRemaining;
                }
                if (fadingOutToSilence)
                {
                    if (fadeOutRemaining > 0)
                    {
                        fadeScale *= static_cast<float>(fadeOutRemaining) / static_cast<float>(kFadeSamples);
                        --fadeOutRemaining;
                    }
                    else
                    {
                        currentSong.reset();
                        fadingOutToSilence = false;
                        fadeScale = 0.0f;
                    }
                }

                s32 outL = static_cast<s32>(mixLeft * fadeScale);
                s32 outR = static_cast<s32>(mixRight * fadeScale);

                if (mixWithExisting)
                {
                    outL += audioData[frame * 2];
                    outR += audioData[frame * 2 + 1];
                }

                audioData[frame * 2] = static_cast<s16>(std::clamp<s32>(outL, -32768, 32767));
                audioData[frame * 2 + 1] = static_cast<s16>(std::clamp<s32>(outR, -32768, 32767));
            }
        }

    private:
        static constexpr int kFadeSamples = 480; // 10ms at 48kHz

        std::shared_ptr<const PreparedSong> currentSong;
        std::array<SeqTrackState, kMaxTracks> tracks {};
        std::array<SynthVoice, kMaxVoices> voices {};
        std::array<s16, 32> variables {};

        u16 tempo = 120;
        u16 tempoRatio = 256;
        s32 tempoCount = 240;
        u8 masterVolume = 127;
        double tickSamplePhase = 0.0;
        u64 voiceCounter = 0;
        u32 rngState = 0x12345678u;

        bool fadingOutToSilence = false;
        int fadeInRemaining = 0;
        int fadeOutRemaining = 0;

        // STRM playback state
        double strmSamplePos = 0.0;
        u32 decodedStrmBlockIndex = 0xFFFFFFFFu;
        std::vector<s16> strmBlockLeft;
        std::vector<s16> strmBlockRight;

        s16 nextRandom(s16 minVal, s16 maxVal) noexcept
        {
            rngState = rngState * 1664525u + 1013904223u;
            if (maxVal <= minVal)
                return minVal;
            const u32 span = static_cast<u32>(static_cast<s32>(maxVal) - static_cast<s32>(minVal) + 1);
            return static_cast<s16>(static_cast<s32>(minVal) + static_cast<s32>((rngState >> 16) % span));
        }

        void resetSongState() noexcept
        {
            for (auto& t : tracks)
                t = SeqTrackState{};
            for (auto& v : voices)
                v = SynthVoice{};
            variables.fill(-1);

            tempo = 120;
            tempoRatio = 256;
            tempoCount = 240;
            masterVolume = 127;
            tickSamplePhase = 0.0;

            strmSamplePos = 0.0;
            decodedStrmBlockIndex = 0xFFFFFFFFu;

            if (currentSong && !currentSong->isStream && !currentSong->sseqData.empty())
            {
                tracks[0].init(0);
            }
        }

        void stepSequencerOneTimerTick() noexcept
        {
            if (!currentSong || currentSong->isStream)
                return;

            tempoCount += (static_cast<s32>(tempo) * static_cast<s32>(tempoRatio)) >> 8;
            while (tempoCount >= 240)
            {
                tempoCount -= 240;
                for (u8 trkIdx = 0; trkIdx < kMaxTracks; ++trkIdx)
                {
                    if (tracks[trkIdx].active)
                        stepTrackTick(trkIdx);
                }
            }

            updateVoicesOneTimerTick();
        }

        void stepTrackTick(u8 trkIdx) noexcept
        {
            SeqTrackState& trk = tracks[trkIdx];
            const u8* data = currentSong->sseqData.data();
            const u32 size = static_cast<u32>(currentSong->sseqData.size());

            if (trk.wait > 0)
            {
                --trk.wait;
                if (trk.wait > 0)
                    return;
            }

            auto readVlq = [&]() -> s32 {
                s32 value = 0;
                for (int i = 0; i < 4 && trk.cur < size; ++i)
                {
                    const u8 b = data[trk.cur++];
                    value = (value << 7) | static_cast<s32>(b & 0x7Fu);
                    if ((b & 0x80u) == 0)
                        break;
                }
                return value;
            };

            int safetyOps = 0;
            while (trk.active && trk.wait == 0 && trk.cur < size && ++safetyOps < 256)
            {
                u8 op = data[trk.cur++];

                bool execute = true;
                if (op == 0xA2) // If prefix
                {
                    execute = trk.cmpFlag;
                    if (trk.cur >= size)
                        break;
                    op = data[trk.cur++];
                }

                s32 overrideArg = 0;
                bool hasOverrideArg = false;

                if (op == 0xA0) // Random prefix
                {
                    if (trk.cur >= size)
                        break;
                    op = data[trk.cur++];
                    if (op < 0x80)
                    {
                        if (trk.cur + 5 > size)
                            break;
                        const u8 vel = data[trk.cur++];
                        const s16 rMin = readS16(data + trk.cur);
                        const s16 rMax = readS16(data + trk.cur + 2);
                        trk.cur += 4;
                        if (execute)
                            triggerNoteOn(trkIdx, op, vel, nextRandom(rMin, rMax));
                        continue;
                    }
                    else if (op >= 0xB0 && op <= 0xBD)
                    {
                        if (trk.cur + 5 > size)
                            break;
                        const u8 varNo = data[trk.cur++];
                        const s16 rMin = readS16(data + trk.cur);
                        const s16 rMax = readS16(data + trk.cur + 2);
                        trk.cur += 4;
                        if (execute)
                            executeVarOp(trk, op, varNo, nextRandom(rMin, rMax));
                        continue;
                    }
                    else
                    {
                        if (trk.cur + 4 > size)
                            break;
                        const s16 rMin = readS16(data + trk.cur);
                        const s16 rMax = readS16(data + trk.cur + 2);
                        trk.cur += 4;
                        overrideArg = nextRandom(rMin, rMax);
                        hasOverrideArg = true;
                    }
                }
                else if (op == 0xA1) // FromVar prefix
                {
                    if (trk.cur >= size)
                        break;
                    op = data[trk.cur++];
                    if (op < 0x80)
                    {
                        if (trk.cur + 2 > size)
                            break;
                        const u8 vel = data[trk.cur++];
                        const u8 varNo = data[trk.cur++] & 0x1Fu;
                        if (execute)
                            triggerNoteOn(trkIdx, op, vel, variables[varNo]);
                        continue;
                    }
                    else if (op >= 0xB0 && op <= 0xBD)
                    {
                        if (trk.cur + 2 > size)
                            break;
                        const u8 dstVar = data[trk.cur++];
                        const u8 srcVar = data[trk.cur++] & 0x1Fu;
                        if (execute)
                            executeVarOp(trk, op, dstVar, variables[srcVar]);
                        continue;
                    }
                    else
                    {
                        if (trk.cur >= size)
                            break;
                        const u8 varNo = data[trk.cur++] & 0x1Fu;
                        overrideArg = variables[varNo];
                        hasOverrideArg = true;
                    }
                }

                if (op < 0x80) // Note-On
                {
                    if (trk.cur >= size)
                        break;
                    const u8 vel = data[trk.cur++];
                    const s32 dur = hasOverrideArg ? overrideArg : readVlq();
                    if (execute)
                        triggerNoteOn(trkIdx, op, vel, dur);
                }
                else if (op == 0x80) // Rest
                {
                    const s32 dur = hasOverrideArg ? overrideArg : readVlq();
                    if (execute)
                        trk.wait = std::max<s32>(0, dur);
                }
                else if (op == 0x81) // Program Change
                {
                    const s32 val = hasOverrideArg ? overrideArg : readVlq();
                    if (execute)
                        trk.program = static_cast<u16>(val & 0x7FFF);
                }
                else if (op == 0x93) // Open Track
                {
                    if (trk.cur + 4 > size)
                        break;
                    const u8 newTrk = data[trk.cur++];
                    const u32 addr = readU24(data + trk.cur);
                    trk.cur += 3;
                    if (execute && newTrk < kMaxTracks && addr < size)
                        tracks[newTrk].init(addr);
                }
                else if (op == 0x94) // Jump
                {
                    if (trk.cur + 3 > size)
                        break;
                    const u32 addr = readU24(data + trk.cur);
                    trk.cur += 3;
                    if (execute && addr < size)
                        trk.cur = addr;
                }
                else if (op == 0x95) // Call
                {
                    if (trk.cur + 3 > size)
                        break;
                    const u32 addr = readU24(data + trk.cur);
                    trk.cur += 3;
                    if (execute && addr < size && trk.callStackDepth < 3)
                    {
                        trk.callStack[trk.callStackDepth] = trk.cur;
                        trk.loopCount[trk.callStackDepth] = 0;
                        trk.callStackDepth++;
                        trk.cur = addr;
                    }
                }
                else if (op >= 0xB0 && op <= 0xBD)
                {
                    if (trk.cur + 3 > size)
                        break;
                    const u8 varNo = data[trk.cur++];
                    const s16 val = readS16(data + trk.cur);
                    trk.cur += 2;
                    if (execute)
                        executeVarOp(trk, op, varNo, val);
                }
                else if (op >= 0xC0 && op <= 0xD6)
                {
                    s32 arg = 0;
                    if (hasOverrideArg)
                    {
                        arg = overrideArg;
                    }
                    else
                    {
                        if (trk.cur >= size)
                            break;
                        arg = data[trk.cur++];
                    }
                    if (!execute)
                        continue;

                    switch (op)
                    {
                    case 0xC0: trk.pan = static_cast<u8>(std::clamp<s32>(arg, 0, 127)); break;
                    case 0xC1: trk.volume = static_cast<u8>(std::clamp<s32>(arg, 0, 127)); break;
                    case 0xC2: masterVolume = static_cast<u8>(std::clamp<s32>(arg, 0, 127)); break;
                    case 0xC3: trk.transpose = static_cast<s8>(arg); break;
                    case 0xC4: trk.pitchBend = static_cast<s8>(arg); break;
                    case 0xC5: trk.pitchBendRange = static_cast<u8>(std::clamp<s32>(arg, 0, 64)); break;
                    case 0xC7: trk.noteWait = (arg != 0); break;
                    case 0xC8:
                        trk.tie = (arg != 0);
                        if (!trk.tie)
                            releaseTrackVoices(trkIdx);
                        break;
                    case 0xC9:
                        trk.portaKey = static_cast<u8>(std::clamp<s32>(arg + trk.transpose, 0, 127));
                        trk.portaEnabled = true;
                        break;
                    case 0xCA: trk.modDepth = static_cast<u8>(arg & 0xFF); break;
                    case 0xCB: trk.modSpeed = static_cast<u8>(arg & 0xFF); break;
                    case 0xCC: trk.modType = static_cast<u8>(arg & 0xFF); break;
                    case 0xCD: trk.modRange = static_cast<u8>(arg & 0xFF); break;
                    case 0xCE: trk.portaEnabled = (arg != 0); break;
                    case 0xCF: trk.portaTime = static_cast<u8>(arg & 0xFF); break;
                    case 0xD0: trk.attack = static_cast<u8>(arg & 0x7F); break;
                    case 0xD1: trk.decay = static_cast<u8>(arg & 0x7F); break;
                    case 0xD2: trk.sustain = static_cast<u8>(arg & 0x7F); break;
                    case 0xD3: trk.release = static_cast<u8>(arg & 0x7F); break;
                    case 0xD4:
                        if (trk.callStackDepth < 3)
                        {
                            trk.callStack[trk.callStackDepth] = trk.cur;
                            trk.loopCount[trk.callStackDepth] = static_cast<u8>(arg & 0xFF);
                            trk.callStackDepth++;
                        }
                        break;
                    case 0xD5: trk.expression = static_cast<u8>(std::clamp<s32>(arg, 0, 127)); break;
                    default: break;
                    }
                }
                else if (op == 0xE0 || op == 0xE1 || op == 0xE3)
                {
                    s32 arg = 0;
                    if (hasOverrideArg)
                    {
                        arg = overrideArg;
                    }
                    else
                    {
                        if (trk.cur + 2 > size)
                            break;
                        arg = (op == 0xE3)
                            ? static_cast<s32>(readS16(data + trk.cur))
                            : static_cast<s32>(readU16(data + trk.cur));
                        trk.cur += 2;
                    }
                    if (!execute)
                        continue;

                    if (op == 0xE0)
                        trk.modDelay = static_cast<u16>(std::clamp<s32>(arg, 0, 0xFFFF));
                    else if (op == 0xE1)
                        tempo = static_cast<u16>(std::clamp<s32>(arg, 16, 1024));
                    else if (op == 0xE3)
                        trk.sweepPitch = static_cast<s16>(arg);
                }
                else if (op == 0xFC) // Loop End
                {
                    if (execute && trk.callStackDepth > 0)
                    {
                        const u8 depth = trk.callStackDepth - 1;
                        u8 count = trk.loopCount[depth];
                        if (count == 0)
                        {
                            trk.cur = trk.callStack[depth];
                        }
                        else if (--count > 0)
                        {
                            trk.loopCount[depth] = count;
                            trk.cur = trk.callStack[depth];
                        }
                        else
                        {
                            trk.callStackDepth--;
                        }
                    }
                }
                else if (op == 0xFD) // Return
                {
                    if (execute)
                    {
                        if (trk.callStackDepth > 0)
                            trk.cur = trk.callStack[--trk.callStackDepth];
                        else
                            trk.active = false;
                    }
                }
                else if (op == 0xFE) // Allocate Tracks
                {
                    if (trk.cur + 2 <= size)
                        trk.cur += 2;
                }
                else if (op == 0xFF) // Fin
                {
                    if (execute)
                    {
                        trk.active = false;
                        releaseTrackVoices(trkIdx);
                    }
                }
                else
                {
                    trk.active = false;
                    break;
                }
            }
        }

        void executeVarOp(SeqTrackState& trk, u8 op, u8 varNo, s16 val) noexcept
        {
            s16& var = variables[varNo & 0x1Fu];
            switch (op)
            {
            case 0xB0: var = val; break;
            case 0xB1: var = static_cast<s16>(var + val); break;
            case 0xB2: var = static_cast<s16>(var - val); break;
            case 0xB3: var = static_cast<s16>(var * val); break;
            case 0xB4: if (val != 0) var = static_cast<s16>(var / val); break;
            case 0xB5:
                var = (val >= 0)
                    ? static_cast<s16>(var << std::min<s16>(val, 15))
                    : static_cast<s16>(var >> std::min<s16>(-val, 15));
                break;
            case 0xB6:
                var = (val >= 0) ? nextRandom(0, val) : nextRandom(val, 0);
                break;
            case 0xB8: trk.cmpFlag = (var == val); break;
            case 0xB9: trk.cmpFlag = (var >= val); break;
            case 0xBA: trk.cmpFlag = (var > val); break;
            case 0xBB: trk.cmpFlag = (var <= val); break;
            case 0xBC: trk.cmpFlag = (var < val); break;
            case 0xBD: trk.cmpFlag = (var != val); break;
            default: break;
            }
        }

        void releaseTrackVoices(u8 trkIdx) noexcept
        {
            for (SynthVoice& v : voices)
            {
                if (v.active && v.trackIndex == trkIdx && v.envPhase != EnvPhase::Release)
                    v.envPhase = EnvPhase::Release;
            }
        }

        void triggerNoteOn(u8 trkIdx, u8 rawKey, u8 velocity, s32 duration) noexcept
        {
            SeqTrackState& trk = tracks[trkIdx];
            if (trk.noteWait)
                trk.wait = std::max<s32>(0, duration);

            const int keyInt = std::clamp<int>(static_cast<int>(rawKey) + static_cast<int>(trk.transpose), 0, 127);
            const u8 key = static_cast<u8>(keyInt);

            if (trk.program >= currentSong->instruments.size())
            {
                trk.portaKey = key;
                return;
            }

            const SbnkInstrument& inst = currentSong->instruments[trk.program];
            const SubInstrument* sub = inst.resolveForKey(key);
            if (!sub || sub->type == 0)
            {
                trk.portaKey = key;
                return;
            }
            if (sub->type == 1 && sub->decodedSampleIndex < 0)
            {
                trk.portaKey = key;
                return;
            }

            SynthVoice* voice = nullptr;
            if (trk.tie)
            {
                for (SynthVoice& v : voices)
                {
                    if (v.active && v.trackIndex == trkIdx && v.envPhase != EnvPhase::Release)
                    {
                        voice = &v;
                        break;
                    }
                }
            }

            const bool isNewVoice = (voice == nullptr);
            if (isNewVoice)
            {
                // Find free voice or steal lowest gain / oldest release voice
                for (SynthVoice& v : voices)
                {
                    if (!v.active)
                    {
                        voice = &v;
                        break;
                    }
                }
                if (!voice)
                {
                    SynthVoice* bestCandidate = &voices[0];
                    for (SynthVoice& v : voices)
                    {
                        if (v.envPhase == EnvPhase::Release && bestCandidate->envPhase != EnvPhase::Release)
                            bestCandidate = &v;
                        else if ((v.envPhase == EnvPhase::Release) == (bestCandidate->envPhase == EnvPhase::Release)
                                 && v.birthOrder < bestCandidate->birthOrder)
                            bestCandidate = &v;
                    }
                    voice = bestCandidate;
                }

                *voice = SynthVoice{};
                voice->active = true;
                voice->trackIndex = trkIdx;
                voice->instType = sub->type;
                voice->sampleIndex = sub->decodedSampleIndex;
                voice->psgDuty = static_cast<u8>(sub->swavNo & 7u);
                voice->noiseLfsr = 0x7FFFu;
                voice->origKey = sub->origKey;
                voice->instPan = sub->pan;
                voice->samplePhase = 0.0;
                voice->birthOrder = ++voiceCounter;

                const u8 atk = (trk.attack != 0xFF) ? trk.attack : sub->attack;
                const u8 dec = (trk.decay != 0xFF) ? trk.decay : sub->decay;
                const u8 sus = (trk.sustain != 0xFF) ? trk.sustain : sub->sustain;
                const u8 rel = (trk.release != 0xFF) ? trk.release : sub->release;

                voice->attackCoeff = calcAttackCoeff(atk);
                voice->decayRate = calcDecayRate(dec);
                voice->sustainLevel = calcSustainDb(sus);
                voice->releaseRate = calcDecayRate(rel);
                voice->envPhase = EnvPhase::Attack;
                voice->envLevel = (voice->attackCoeff == 0) ? 0 : -92544;
                voice->modDelayCnt = 0;
                voice->lfoPhase = 0;
            }

            voice->noteKey = key;
            voice->velocity = std::clamp<u8>(velocity, 0, 127);
            voice->noteTicksRemaining = trk.tie ? -1 : ((duration > 0) ? duration : -1);

            if (trk.portaEnabled)
            {
                const s32 diff64 = (static_cast<s32>(trk.portaKey) - static_cast<s32>(key)) * 64 + trk.sweepPitch;
                voice->sweepTarget = diff64;
                voice->sweepCurrent = 0;
                voice->sweepCnt = 0;
                voice->sweepLen = (trk.portaTime > 0) ? (static_cast<s32>(trk.portaTime) * static_cast<s32>(trk.portaTime) >> 6) : duration;
            }
            else if (trk.sweepPitch != 0)
            {
                voice->sweepTarget = trk.sweepPitch;
                voice->sweepCurrent = 0;
                voice->sweepCnt = 0;
                voice->sweepLen = duration;
            }
            else
            {
                voice->sweepTarget = 0;
                voice->sweepCurrent = 0;
                voice->sweepLen = 0;
            }

            trk.portaKey = key;
            updateVoicePitchAndGain(*voice);
        }

        void updateVoicePitchAndGain(SynthVoice& v) noexcept
        {
            const SeqTrackState& trk = tracks[v.trackIndex];

            // Envelope linear gain
            const float envGain = (v.envLevel <= -92544)
                ? 0.0f
                : std::exp2(static_cast<float>(v.envLevel) * (3.321928095f / (20.0f * 1024.0f)));

            const float seqVol = static_cast<float>(currentSong->seqVolume) * (1.0f / 127.0f);
            const float mstVol = static_cast<float>(masterVolume) * (1.0f / 127.0f);
            const float trkVol = static_cast<float>(trk.volume) * (1.0f / 127.0f);
            const float trkExp = static_cast<float>(trk.expression) * (1.0f / 127.0f);
            const float velVol = static_cast<float>(v.velocity) * (1.0f / 127.0f);

            v.currentLinearGain = seqVol * mstVol * trkVol * trkExp * velVol * envGain;

            // Pitch calculation in 1/64 semitones
            double pitch64 = static_cast<double>(static_cast<int>(v.noteKey) - static_cast<int>(v.origKey)) * 64.0;
            pitch64 += static_cast<double>(trk.pitchBend) * static_cast<double>(trk.pitchBendRange) * 0.5;

            if (v.sweepTarget != 0 && v.sweepCnt < v.sweepLen)
            {
                pitch64 += static_cast<double>(v.sweepTarget - v.sweepCurrent);
            }

            if (trk.modDepth > 0 && v.modDelayCnt >= trk.modDelay && trk.modType == 0)
            {
                const double lfoAngle = static_cast<double>(v.lfoPhase) * (2.0 * 3.14159265358979323846 / 65536.0);
                pitch64 += std::sin(lfoAngle) * static_cast<double>(trk.modDepth) * static_cast<double>(trk.modRange) * 0.5;
            }

            if (v.instType == 1 && v.sampleIndex >= 0 && static_cast<size_t>(v.sampleIndex) < currentSong->samples.size())
            {
                const double baseRate = currentSong->samples[v.sampleIndex].baseSampleRate;
                const double effectiveRate = baseRate * std::exp2(pitch64 / 768.0);
                v.sampleStep = effectiveRate / kOutputSampleRate;
            }
            else if (v.instType == 2) // PSG: 8 steps per cycle
            {
                const double freqHz = 440.0 * std::exp2((static_cast<double>(static_cast<int>(v.noteKey) - 69) * 64.0
                    + pitch64 - static_cast<double>(static_cast<int>(v.noteKey) - static_cast<int>(v.origKey)) * 64.0) / 768.0);
                v.sampleStep = (freqHz * 8.0) / kOutputSampleRate;
            }
            else if (v.instType == 3) // Noise
            {
                const double freqHz = 440.0 * 16.0 * std::exp2(pitch64 / 768.0);
                v.sampleStep = freqHz / kOutputSampleRate;
            }
        }

        void updateVoicesOneTimerTick() noexcept
        {
            for (SynthVoice& v : voices)
            {
                if (!v.active)
                    continue;

                if (v.noteTicksRemaining > 0)
                {
                    --v.noteTicksRemaining;
                    if (v.noteTicksRemaining == 0)
                        v.envPhase = EnvPhase::Release;
                }

                // Envelope step at 192Hz
                switch (v.envPhase)
                {
                case EnvPhase::Attack:
                    if (v.attackCoeff == 0)
                    {
                        v.envLevel = 0;
                        v.envPhase = EnvPhase::Decay;
                    }
                    else
                    {
                        v.envLevel = static_cast<s32>((static_cast<s64>(v.envLevel) * v.attackCoeff) >> 8);
                        if (v.envLevel >= -256)
                        {
                            v.envLevel = 0;
                            v.envPhase = EnvPhase::Decay;
                        }
                    }
                    break;
                case EnvPhase::Decay:
                    v.envLevel -= v.decayRate;
                    if (v.envLevel <= v.sustainLevel)
                    {
                        v.envLevel = v.sustainLevel;
                        v.envPhase = EnvPhase::Sustain;
                    }
                    break;
                case EnvPhase::Sustain:
                    if (v.envLevel <= -92544)
                        v.active = false;
                    break;
                case EnvPhase::Release:
                    v.envLevel -= v.releaseRate;
                    if (v.envLevel <= -92544)
                    {
                        v.envLevel = -92544;
                        v.active = false;
                        continue;
                    }
                    break;
                default:
                    v.active = false;
                    continue;
                }

                // Sweep / Portamento step
                if (v.sweepTarget != 0 && v.sweepCnt < v.sweepLen && v.sweepLen > 0)
                {
                    ++v.sweepCnt;
                    v.sweepCurrent = static_cast<s32>((static_cast<s64>(v.sweepTarget) * v.sweepCnt) / v.sweepLen);
                }

                // LFO step
                const SeqTrackState& trk = tracks[v.trackIndex];
                if (v.modDelayCnt < trk.modDelay)
                    ++v.modDelayCnt;
                else if (trk.modDepth > 0)
                    v.lfoPhase = static_cast<u16>(v.lfoPhase + static_cast<u16>(trk.modSpeed) * 64u);

                updateVoicePitchAndGain(v);
            }
        }

        // STRM block decoding and playback
        bool ensureStrmBlockDecoded(u32 blockIdx) noexcept
        {
            if (decodedStrmBlockIndex == blockIdx)
                return true;
            if (!currentSong || !currentSong->strm)
                return false;

            const PreparedStrm& strm = *currentSong->strm;
            if (blockIdx >= strm.numBlocks)
                return false;

            const bool isLast = (blockIdx + 1 == strm.numBlocks);
            const u32 curBlockBytes = isLast ? strm.lastBlockSize : strm.blockSize;
            const u32 curBlockSamples = isLast ? strm.lastBlockSamples : strm.samplesPerBlock;
            if (curBlockBytes == 0 || curBlockSamples == 0)
                return false;

            const u64 blockStartOffset = static_cast<u64>(strm.dataOffset)
                + static_cast<u64>(blockIdx) * static_cast<u64>(strm.blockSize) * static_cast<u64>(strm.numChannels);
            if (blockStartOffset + static_cast<u64>(curBlockBytes) * strm.numChannels > strm.strmData.size())
                return false;

            strmBlockLeft.resize(curBlockSamples);
            strmBlockRight.resize(curBlockSamples);

            auto decodeChannelBlock = [&](const u8* ptr, std::vector<s16>& dst) {
                if (strm.waveType == 0) // PCM8
                {
                    for (u32 i = 0; i < curBlockSamples && i < curBlockBytes; ++i)
                        dst[i] = static_cast<s16>(static_cast<s8>(ptr[i]) * 256);
                }
                else if (strm.waveType == 1) // PCM16
                {
                    for (u32 i = 0; i < curBlockSamples && (i * 2 + 1) < curBlockBytes; ++i)
                        dst[i] = readS16(ptr + i * 2);
                }
                else if (strm.waveType == 2 && curBlockBytes > 4) // IMA-ADPCM
                {
                    s32 val = static_cast<s32>(readS16(ptr));
                    s32 idx = std::clamp<int>(readU16(ptr + 2) & 0x7Fu, 0, 88);
                    u32 outIdx = 0;
                    for (u32 b = 4; b < curBlockBytes && outIdx < curBlockSamples; ++b)
                    {
                        const u8 byteVal = ptr[b];
                        for (int n = 0; n < 2 && outIdx < curBlockSamples; ++n)
                        {
                            const u8 nib = (n == 0) ? (byteVal & 0x0Fu) : (byteVal >> 4);
                            const u16 step = melonDS::SPUChannel::ADPCMTable[idx];
                            u16 diff = step >> 3;
                            if (nib & 1) diff += (step >> 2);
                            if (nib & 2) diff += (step >> 1);
                            if (nib & 4) diff += step;
                            if (nib & 8) { val -= diff; if (val < -0x7FFF) val = -0x7FFF; }
                            else         { val += diff; if (val > 0x7FFF)  val = 0x7FFF;  }
                            idx += melonDS::SPUChannel::ADPCMIndexTable[nib & 7];
                            if (idx < 0) idx = 0;
                            else if (idx > 88) idx = 88;
                            dst[outIdx++] = static_cast<s16>(val);
                        }
                    }
                }
            };

            const u8* blockPtr = strm.strmData.data() + blockStartOffset;
            decodeChannelBlock(blockPtr, strmBlockLeft);
            if (strm.numChannels >= 2)
                decodeChannelBlock(blockPtr + curBlockBytes, strmBlockRight);
            else
                strmBlockRight = strmBlockLeft;

            decodedStrmBlockIndex = blockIdx;
            return true;
        }

        void renderStream(s16* audioData, int numFrames, bool mixWithExisting) noexcept
        {
            const PreparedStrm& strm = *currentSong->strm;
            const double step = static_cast<double>(strm.sampleRate) / kOutputSampleRate;
            const float gain = (static_cast<float>(strm.volume) / 127.0f) * 0.7f;

            for (int frame = 0; frame < numFrames; ++frame)
            {
                if (strmSamplePos >= static_cast<double>(strm.numSamples))
                {
                    if (strm.loop && strm.loopStartSample < strm.numSamples)
                        strmSamplePos = static_cast<double>(strm.loopStartSample);
                    else
                    {
                        if (!mixWithExisting)
                        {
                            audioData[frame * 2] = 0;
                            audioData[frame * 2 + 1] = 0;
                        }
                        continue;
                    }
                }

                const u32 globalSample = static_cast<u32>(strmSamplePos);
                const u32 blockIdx = globalSample / strm.samplesPerBlock;
                const u32 sampleInBlock = globalSample % strm.samplesPerBlock;

                s32 outL = 0;
                s32 outR = 0;
                if (ensureStrmBlockDecoded(blockIdx) && sampleInBlock < strmBlockLeft.size())
                {
                    outL = static_cast<s32>(static_cast<float>(strmBlockLeft[sampleInBlock]) * gain);
                    outR = static_cast<s32>(static_cast<float>(strmBlockRight[sampleInBlock]) * gain);
                }

                if (mixWithExisting)
                {
                    outL += audioData[frame * 2];
                    outR += audioData[frame * 2 + 1];
                }

                audioData[frame * 2] = static_cast<s16>(std::clamp<s32>(outL, -32768, 32767));
                audioData[frame * 2 + 1] = static_cast<s16>(std::clamp<s32>(outR, -32768, 32767));
                strmSamplePos += step;
            }
        }
    };

    // =========================================================================
    // SDAT ROM Database & Live Song Detector
    // =========================================================================

    struct SdatFileSpan
    {
        u32 romOffset = 0;
        u32 size = 0;
    };

    struct SdatSeqEntry
    {
        u32 seqId = 0;
        u16 fileId = 0;
        u16 bankId = 0;
        u8 vol = 127;
        u8 cpr = 64;
        u8 ppr = 64;
        u8 ply = 0;
        u32 romOffset = 0;
        u32 romSize = 0;
        u32 dataOffset = 0x1C;
        u32 dataSize = 0;
        u64 bytecodeHash = 0;
        bool hasLoopJump = false;
    };

    struct SdatBankEntry
    {
        u32 bankId = 0;
        u16 fileId = 0;
        std::array<u16, 4> waveArcIds {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
        u32 romOffset = 0;
        u32 romSize = 0;
    };

    struct SdatWaveArcEntry
    {
        u32 waveArcId = 0;
        u16 fileId = 0;
        u32 romOffset = 0;
        u32 romSize = 0;
    };

    struct SdatStrmEntry
    {
        u32 strmId = 0;
        u16 fileId = 0;
        u8 vol = 127;
        u8 ply = 0;
        u32 romOffset = 0;
        u32 romSize = 0;
        bool isLoopingOrLongBgm = false;
    };

    struct SdatArchive
    {
        u32 sdatRomOffset = 0;
        u32 sdatSize = 0;
        std::vector<SdatFileSpan> fatFiles;
        std::vector<SdatSeqEntry> sequences;
        std::vector<SdatBankEntry> banks;
        std::vector<SdatWaveArcEntry> waveArcs;
        std::vector<SdatStrmEntry> streams;
    };

    struct WramTrackSlot
    {
        u32 wramIndex = 0;
        u32 baseAddr = 0;
        u64 neighborhoodHash = 0;
        u32 changeCount = 0;
        u32 lastChangeFrame = 0;
        u32 firstSeenFrame = 0;
        u32 curOffset = 0;
    };

    struct SpuChannelStartInfo
    {
        u8 format = 0;
        u8 duty = 0;
        u32 loopPos = 0;
        u32 length = 0;
        u32 fifo0 = 0;
        u32 fifo1 = 0;
    };

    std::atomic<bool> normalSpeedMusicEnabled {false};
    std::atomic<bool> sfxEnabled {false};
    std::atomic<bool> wasInFastForward {false};

    std::mutex synthMutex;
    CustomMusicSynthesizer synthesizer;

    // Emulator-thread state
    const u8* currentRomPtr = nullptr;
    u32 currentRomLength = 0;
    std::vector<SdatArchive> sdatArchives;
    std::vector< std::shared_ptr<PreparedSong> > songCache;

    u32 emulatorFrameCounter = 0;
    std::vector<WramTrackSlot> wramTrackSlots;
    u64 activeBgmSongKey = 0;
    u32 activeBgmStartFrame = 0;
    u32 activeBgmMainRamBase = 0;

    // ROM read tracking for STRM / SSEQ fallback
    std::atomic<u32> recentRomReadAddr {0};
    std::atomic<u32> recentRomReadLen {0};
    std::atomic<u32> recentRomReadSeqKey {0};
    std::atomic<u32> recentRomReadStrmKey {0};
    u32 lastStrmKeySeen = 0;
    u32 lastStrmSeenFrame = 0;
    u32 strmReadHitCount = 0;

    // SPU channel BGM vs SFX classification (emulator thread)
    std::vector<SampleSignature> currentBgmSignatures;
    u8 currentBgmPsgDutyMask = 0;
    bool currentBgmUsesNoise = false;
    bool currentBgmIsStream = false;
    std::atomic<bool> hasActiveBgmForSpuFilter {false};
    std::array<SpuChannelStartInfo, 16> channelStartInfos {};
    std::array<std::atomic<bool>, 16> channelIsBgm {};

    bool classifySpuChannelAsBgm(u32 channelNum, const SpuChannelStartInfo& info) noexcept
    {
        if (!hasActiveBgmForSpuFilter.load(std::memory_order_relaxed))
            return false;

        if (currentBgmIsStream)
        {
            // Streamed BGM plays on looping PCM/ADPCM channels 0..3
            return channelNum < 4 && info.format != 3 && info.loopPos == 0 && info.length > 0;
        }

        if (info.format == 3) // PSG or Noise
        {
            if (channelNum >= 14)
                return currentBgmUsesNoise;
            if (channelNum >= 8)
                return (currentBgmPsgDutyMask & static_cast<u8>(1u << (info.duty & 7u))) != 0;
            return false;
        }

        for (const SampleSignature& sig : currentBgmSignatures)
        {
            if (sig.matches(info.format, info.loopPos, info.length, info.fifo0, info.fifo1))
                return true;
        }
        return false;
    }

    void reclassifyAllSpuChannels() noexcept
    {
        for (u32 ch = 0; ch < 16; ++ch)
        {
            const bool isBgm = classifySpuChannelAsBgm(ch, channelStartInfos[ch]);
            channelIsBgm[ch].store(isBgm, std::memory_order_relaxed);
        }
    }

    void parseSdatArchiveAt(const u8* rom, u32 romLen, u32 sdatOffset)
    {
        if (sdatOffset + 0x40 > romLen)
            return;
        const u8* sdat = rom + sdatOffset;
        if (readU32(sdat) != kMagicSDAT || readU16(sdat + 4) != 0xFEFFu)
            return;

        const u32 sdatSize = std::min<u32>(readU32(sdat + 0x08), romLen - sdatOffset);
        const u32 infoOffset = readU32(sdat + 0x18);
        const u32 fatOffset = readU32(sdat + 0x20);
        if (infoOffset == 0 || fatOffset == 0 || infoOffset + 0x40 > sdatSize || fatOffset + 0x0C > sdatSize)
            return;

        const u8* infoBase = sdat + infoOffset;
        const u8* fatBase = sdat + fatOffset;
        if (readU32(infoBase) != kMagicINFO || readU32(fatBase) != kMagicFAT)
            return;

        SdatArchive archive;
        archive.sdatRomOffset = sdatOffset;
        archive.sdatSize = sdatSize;

        const u32 numFatFiles = std::min<u32>(readU32(fatBase + 0x08), 16384u);
        archive.fatFiles.resize(numFatFiles);
        for (u32 f = 0; f < numFatFiles; ++f)
        {
            const u32 recOff = fatOffset + 0x0Cu + f * 0x10u;
            if (recOff + 0x08 > sdatSize)
                break;
            const u32 fileOff = readU32(sdat + recOff);
            const u32 fileSz = readU32(sdat + recOff + 4);
            if (fileOff > 0 && sdatOffset + fileOff + fileSz <= romLen)
            {
                archive.fatFiles[f] = {sdatOffset + fileOff, fileSz};
            }
        }

        // Parse WAVEARC records
        const u32 waveArcListRel = readU32(infoBase + 0x14);
        if (waveArcListRel > 0 && infoOffset + waveArcListRel + 4 <= sdatSize)
        {
            const u32 count = std::min<u32>(readU32(infoBase + waveArcListRel), 4096u);
            archive.waveArcs.resize(count);
            for (u32 i = 0; i < count; ++i)
            {
                const u32 ptrOff = infoOffset + waveArcListRel + 4u + i * 4u;
                if (ptrOff + 4 > sdatSize)
                    break;
                const u32 entryRel = readU32(sdat + ptrOff);
                if (entryRel == 0 || infoOffset + entryRel + 2 > sdatSize)
                    continue;
                const u16 fileId = readU16(infoBase + entryRel);
                if (fileId < archive.fatFiles.size() && archive.fatFiles[fileId].size >= 0x3C)
                {
                    archive.waveArcs[i] = {
                        i,
                        fileId,
                        archive.fatFiles[fileId].romOffset,
                        archive.fatFiles[fileId].size
                    };
                }
            }
        }

        // Parse BANK records
        const u32 bankListRel = readU32(infoBase + 0x10);
        if (bankListRel > 0 && infoOffset + bankListRel + 4 <= sdatSize)
        {
            const u32 count = std::min<u32>(readU32(infoBase + bankListRel), 4096u);
            archive.banks.resize(count);
            for (u32 i = 0; i < count; ++i)
            {
                const u32 ptrOff = infoOffset + bankListRel + 4u + i * 4u;
                if (ptrOff + 4 > sdatSize)
                    break;
                const u32 entryRel = readU32(sdat + ptrOff);
                if (entryRel == 0 || infoOffset + entryRel + 12 > sdatSize)
                    continue;
                const u8* entry = infoBase + entryRel;
                const u16 fileId = readU16(entry + 0);
                if (fileId < archive.fatFiles.size() && archive.fatFiles[fileId].size >= 0x3C)
                {
                    SdatBankEntry bank {};
                    bank.bankId = i;
                    bank.fileId = fileId;
                    for (int w = 0; w < 4; ++w)
                        bank.waveArcIds[w] = readU16(entry + 4 + w * 2);
                    bank.romOffset = archive.fatFiles[fileId].romOffset;
                    bank.romSize = archive.fatFiles[fileId].size;
                    archive.banks[i] = bank;
                }
            }
        }

        // Parse SEQ records
        const u32 seqListRel = readU32(infoBase + 0x08);
        if (seqListRel > 0 && infoOffset + seqListRel + 4 <= sdatSize)
        {
            const u32 count = std::min<u32>(readU32(infoBase + seqListRel), 4096u);
            archive.sequences.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                const u32 ptrOff = infoOffset + seqListRel + 4u + i * 4u;
                if (ptrOff + 4 > sdatSize)
                    break;
                const u32 entryRel = readU32(sdat + ptrOff);
                if (entryRel == 0 || infoOffset + entryRel + 10 > sdatSize)
                    continue;
                const u8* entry = infoBase + entryRel;
                const u16 fileId = readU16(entry + 0);
                if (fileId >= archive.fatFiles.size())
                    continue;
                const SdatFileSpan span = archive.fatFiles[fileId];
                if (span.size <= 0x1C || span.romOffset + span.size > romLen)
                    continue;
                const u8* sseqPtr = rom + span.romOffset;
                if (readU32(sseqPtr) != kMagicSSEQ || readU32(sseqPtr + 0x10) != kMagicDATA)
                    continue;
                const u32 dataOff = readU32(sseqPtr + 0x18);
                if (dataOff >= span.size)
                    continue;
                const u32 dataSz = span.size - dataOff;

                SdatSeqEntry seq {};
                seq.seqId = i;
                seq.fileId = fileId;
                seq.bankId = readU16(entry + 4);
                seq.vol = readU8(entry + 6);
                seq.cpr = readU8(entry + 7);
                seq.ppr = readU8(entry + 8);
                seq.ply = readU8(entry + 9);
                seq.romOffset = span.romOffset;
                seq.romSize = span.size;
                seq.dataOffset = dataOff;
                seq.dataSize = dataSz;
                seq.bytecodeHash = fnv1a64(sseqPtr + dataOff, std::min<u32>(dataSz, 256u));

                u32 dummyMinLoop = 0;
                std::vector<bool> dummyProgs;
                analyzeSseqBytecode(sseqPtr + dataOff, dataSz, seq.hasLoopJump, dummyMinLoop, dummyProgs);
                archive.sequences.push_back(seq);
            }
        }

        // Parse STRM records
        const u32 strmListRel = readU32(infoBase + 0x24);
        if (strmListRel > 0 && infoOffset + strmListRel + 4 <= sdatSize)
        {
            const u32 count = std::min<u32>(readU32(infoBase + strmListRel), 4096u);
            for (u32 i = 0; i < count; ++i)
            {
                const u32 ptrOff = infoOffset + strmListRel + 4u + i * 4u;
                if (ptrOff + 4 > sdatSize)
                    break;
                const u32 entryRel = readU32(sdat + ptrOff);
                if (entryRel == 0 || infoOffset + entryRel + 7 > sdatSize)
                    continue;
                const u8* entry = infoBase + entryRel;
                const u16 fileId = readU16(entry + 0);
                if (fileId >= archive.fatFiles.size())
                    continue;
                const SdatFileSpan span = archive.fatFiles[fileId];
                if (span.size < 0x68 || span.romOffset + span.size > romLen)
                    continue;
                const u8* strmPtr = rom + span.romOffset;
                if (readU32(strmPtr) != kMagicSTRM || readU32(strmPtr + 0x10) != kMagicHEAD)
                    continue;

                const bool loopFlag = readU8(strmPtr + 0x19) != 0;
                const u16 sampleRate = std::max<u16>(readU16(strmPtr + 0x1C), 1);
                const u32 numSamples = readU32(strmPtr + 0x24);

                SdatStrmEntry strm {};
                strm.strmId = i;
                strm.fileId = fileId;
                strm.vol = readU8(entry + 4);
                strm.ply = readU8(entry + 6);
                strm.romOffset = span.romOffset;
                strm.romSize = span.size;
                strm.isLoopingOrLongBgm = loopFlag || (numSamples / sampleRate >= 6);
                archive.streams.push_back(strm);
            }
        }

        if (!archive.sequences.empty() || !archive.streams.empty())
            sdatArchives.push_back(std::move(archive));
    }

    std::shared_ptr<PreparedSong> getOrBuildSdatSseqSong(
        size_t archiveIdx,
        const SdatSeqEntry& seq,
        const u8* mainRam,
        u32 mainRamSize,
        u32 mainRamMask)
    {
        const u64 songKey = (1ULL << 62) | (static_cast<u64>(archiveIdx) << 32) | static_cast<u64>(seq.seqId);
        for (const auto& cached : songCache)
        {
            if (cached->songKey == songKey)
                return cached;
        }

        const SdatArchive& archive = sdatArchives[archiveIdx];
        if (seq.bankId >= archive.banks.size() || archive.banks[seq.bankId].romSize == 0)
            return nullptr;

        const SdatBankEntry& bank = archive.banks[seq.bankId];
        const u8* sseqData = currentRomPtr + seq.romOffset + seq.dataOffset;
        const u8* sbnkFile = currentRomPtr + bank.romOffset;

        std::array<const u8*, 4> swarPtrs {nullptr, nullptr, nullptr, nullptr};
        std::array<u32, 4> swarSizes {0, 0, 0, 0};

        for (int w = 0; w < 4; ++w)
        {
            const u16 waveArcId = bank.waveArcIds[w];
            if (waveArcId < archive.waveArcs.size() && archive.waveArcs[waveArcId].romSize >= 0x40)
            {
                swarPtrs[w] = currentRomPtr + archive.waveArcs[waveArcId].romOffset;
                swarSizes[w] = archive.waveArcs[waveArcId].romSize;
            }
        }

        // Also check MainRAM for any dynamically assigned SWAR links in the loaded SBNK
        if (mainRam && mainRamSize >= 0x40)
        {
            for (u32 off = 0; off + bank.romSize <= mainRamSize; off += 4)
            {
                if (readU32(mainRam + off) == kMagicSBNK
                    && readU32(mainRam + off + 0x08) == bank.romSize
                    && readU32(mainRam + off + 0x38) == readU32(sbnkFile + 0x38))
                {
                    for (int w = 0; w < 4; ++w)
                    {
                        if (swarPtrs[w] != nullptr)
                            continue;
                        const u32 swarRamPtr = readU32(mainRam + off + 0x18 + w * 8);
                        if ((swarRamPtr & 0xFF000000u) == 0x02000000u)
                        {
                            const u32 swarOff = swarRamPtr & mainRamMask;
                            if (swarOff + 0x40 <= mainRamSize && readU32(mainRam + swarOff) == kMagicSWAR)
                            {
                                const u32 sz = readU32(mainRam + swarOff + 0x08);
                                if (sz >= 0x40 && swarOff + sz <= mainRamSize)
                                {
                                    swarPtrs[w] = mainRam + swarOff;
                                    swarSizes[w] = sz;
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }

        auto built = buildPreparedSseqSong(
            songKey,
            sseqData,
            seq.dataSize,
            seq.vol,
            sbnkFile,
            bank.romSize,
            swarPtrs,
            swarSizes);
        if (built)
        {
            if (songCache.size() >= 8)
                songCache.erase(songCache.begin());
            songCache.push_back(built);
        }
        return built;
    }

    std::shared_ptr<PreparedSong> buildMainRamFallbackSong(
        u32 sseqRamOff,
        u32 sseqDataOff,
        u32 sseqDataSize,
        const u8* mainRam,
        u32 mainRamSize,
        u32 mainRamMask,
        const u8* wram7,
        u32 wram7Size)
    {
        const u64 bytecodeHash = fnv1a64(mainRam + sseqRamOff + sseqDataOff, std::min<u32>(sseqDataSize, 256u));
        const u64 songKey = (2ULL << 62) | bytecodeHash;
        for (const auto& cached : songCache)
        {
            if (cached->songKey == songKey)
                return cached;
        }

        // Find active SBNK pointer in ARM7WRAM
        u32 bestSbnkOff = 0xFFFFFFFFu;
        u32 bestSbnkSize = 0;
        const u32 wramWords = wram7Size >> 2;
        for (u32 i = 0; i < wramWords; ++i)
        {
            const u32 ptr = readU32(wram7 + i * 4);
            if ((ptr & 0xFF000003u) != 0x02000000u)
                continue;
            const u32 off = ptr & mainRamMask;
            if (off + 0x40 <= mainRamSize
                && readU32(mainRam + off) == kMagicSBNK
                && readU32(mainRam + off + 0x10) == kMagicDATA)
            {
                const u32 sz = readU32(mainRam + off + 0x08);
                if (sz >= 0x40 && off + sz <= mainRamSize)
                {
                    bestSbnkOff = off;
                    bestSbnkSize = sz;
                    break;
                }
            }
        }

        if (bestSbnkOff == 0xFFFFFFFFu)
            return nullptr;

        std::array<const u8*, 4> swarPtrs {nullptr, nullptr, nullptr, nullptr};
        std::array<u32, 4> swarSizes {0, 0, 0, 0};
        for (int w = 0; w < 4; ++w)
        {
            const u32 swarRamPtr = readU32(mainRam + bestSbnkOff + 0x18 + w * 8);
            if ((swarRamPtr & 0xFF000000u) == 0x02000000u)
            {
                const u32 swarOff = swarRamPtr & mainRamMask;
                if (swarOff + 0x40 <= mainRamSize
                    && readU32(mainRam + swarOff) == kMagicSWAR
                    && readU32(mainRam + swarOff + 0x10) == kMagicDATA)
                {
                    const u32 sz = readU32(mainRam + swarOff + 0x08);
                    if (sz >= 0x40 && swarOff + sz <= mainRamSize)
                    {
                        swarPtrs[w] = mainRam + swarOff;
                        swarSizes[w] = sz;
                    }
                }
            }
        }

        auto built = buildPreparedSseqSong(
            songKey,
            mainRam + sseqRamOff + sseqDataOff,
            sseqDataSize,
            127,
            mainRam + bestSbnkOff,
            bestSbnkSize,
            swarPtrs,
            swarSizes);
        if (built)
        {
            if (songCache.size() >= 8)
                songCache.erase(songCache.begin());
            songCache.push_back(built);
        }
        return built;
    }

    void activatePreparedSong(const std::shared_ptr<PreparedSong>& song, u32 elapsedFrames)
    {
        if (!song)
            return;

        currentBgmSignatures = song->bgmSignatures;
        currentBgmPsgDutyMask = song->bgmPsgDutyMask;
        currentBgmUsesNoise = song->bgmUsesNoise;
        currentBgmIsStream = song->isStream;
        hasActiveBgmForSpuFilter.store(true, std::memory_order_release);
        reclassifyAllSpuChannels();

        const u32 initialTicks192 = (elapsedFrames * 192u) / 60u;
        std::lock_guard<std::mutex> lock(synthMutex);
        synthesizer.setSong(song, initialTicks192);
    }

    void deactivateCurrentSong()
    {
        activeBgmSongKey = 0;
        activeBgmMainRamBase = 0;
        hasActiveBgmForSpuFilter.store(false, std::memory_order_release);
        currentBgmSignatures.clear();
        currentBgmPsgDutyMask = 0;
        currentBgmUsesNoise = false;
        currentBgmIsStream = false;
        reclassifyAllSpuChannels();

        std::lock_guard<std::mutex> lock(synthMutex);
        synthesizer.stopSong();
    }
} // anonymous namespace

void setNormalSpeedMusicEnabled(bool enabled) noexcept
{
    normalSpeedMusicEnabled.store(enabled, std::memory_order_release);
}

bool isNormalSpeedMusicEnabled() noexcept
{
    return normalSpeedMusicEnabled.load(std::memory_order_acquire);
}

void setSfxEnabled(bool enabled) noexcept
{
    sfxEnabled.store(enabled, std::memory_order_release);
}

bool isSfxEnabled() noexcept
{
    return sfxEnabled.load(std::memory_order_acquire);
}

void onRomLoaded(const melonDS::u8* rom, melonDS::u32 romLength) noexcept
{
    onShutdown();
    if (!rom || romLength < 0x200)
        return;

    currentRomPtr = rom;
    currentRomLength = romLength;

    // 1. Check NDS ROM FAT table for SDAT archives
    const u32 fatOffset = readU32(rom + 0x48);
    const u32 fatSize = readU32(rom + 0x4C);
    if (fatOffset >= 0x200 && fatSize >= 8 && fatOffset + fatSize <= romLength)
    {
        const u32 numEntries = fatSize / 8u;
        for (u32 i = 0; i < numEntries; ++i)
        {
            const u32 start = readU32(rom + fatOffset + i * 8u);
            const u32 end = readU32(rom + fatOffset + i * 8u + 4u);
            if (start < romLength && end <= romLength && end > start && (end - start) >= 0x40)
            {
                if (readU32(rom + start) == kMagicSDAT && readU32(rom + start + 4) == 0x0100FEFFu)
                {
                    parseSdatArchiveAt(rom, romLength, start);
                }
            }
        }
    }

    // 2. Fallback scan at 32-byte alignment if FAT didn't directly expose SDAT
    if (sdatArchives.empty())
    {
        for (u32 off = 0x200; off + 0x40 <= romLength; off += 32)
        {
            if (readU32(rom + off) == kMagicSDAT && readU32(rom + off + 4) == 0x0100FEFFu)
            {
                parseSdatArchiveAt(rom, romLength, off);
            }
        }
    }

    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Info,
        "FastForwardMusic: indexed %zu SDAT archive(s) in ROM\n",
        sdatArchives.size());

    // Games using the DSE sound engine (Pokemon Mystery Dungeon, Professor Layton 2-4, ...)
    // are detected from their smdl/swdl files and handled by FastForwardMusicDse.
    Dse::onRomLoaded(rom, romLength);
}

void onRomRead(melonDS::u32 addr, melonDS::u32 len) noexcept
{
    if (sdatArchives.empty() || len == 0)
        return;

    for (size_t a = 0; a < sdatArchives.size(); ++a)
    {
        const SdatArchive& archive = sdatArchives[a];
        if (addr < archive.sdatRomOffset || addr >= archive.sdatRomOffset + archive.sdatSize)
            continue;

        for (const SdatStrmEntry& strm : archive.streams)
        {
            if (strm.isLoopingOrLongBgm && addr >= strm.romOffset && addr < strm.romOffset + strm.romSize)
            {
                const u32 key = (static_cast<u32>(a + 1) << 16) | (strm.strmId & 0xFFFFu);
                recentRomReadStrmKey.store(key, std::memory_order_relaxed);
                break;
            }
        }

        for (const SdatSeqEntry& seq : archive.sequences)
        {
            if ((seq.hasLoopJump || (seq.ply == 0 && seq.dataSize >= 128))
                && addr >= seq.romOffset && addr < seq.romOffset + seq.romSize)
            {
                const u32 key = (static_cast<u32>(a + 1) << 16) | (seq.seqId & 0xFFFFu);
                recentRomReadSeqKey.store(key, std::memory_order_relaxed);
                break;
            }
        }
    }
}

void onEmulatorFrame(melonDS::NDS& nds) noexcept
{
    if (!normalSpeedMusicEnabled.load(std::memory_order_relaxed))
    {
        // Don't spend time scanning memory while the option is off.
        if (activeBgmSongKey != 0)
            deactivateCurrentSong();
        wramTrackSlots.clear();
        Dse::disable();
        return;
    }

    Dse::onEmulatorFrame(nds, MelonDSAndroid::isFastForwardActive());
    if (sdatArchives.empty() && Dse::isDseGame())
        return;

    ++emulatorFrameCounter;
    if ((emulatorFrameCounter & 1u) != 0)
        return;

    const u8* mainRam = nds.MainRAM;
    const u32 mainRamMask = nds.MainRAMMask;
    const u32 mainRamSize = mainRamMask + 1u;
    if (!mainRam || mainRamSize < 0x1000)
        return;

    // Scan ARM7WRAM (64KB) and SharedWRAM (32KB) for SNDTrack (base, cur) pointers
    auto scanWramRegion = [&](const u8* wram, u32 wramBytes, u32 wramIndexBase) {
        if (!wram || wramBytes < 32)
            return;
        const u32 numWords = (wramBytes >> 2);
        for (u32 i = 2; i + 2 < numWords; ++i)
        {
            const u32 baseCandidate = readU32(wram + i * 4u);
            if ((baseCandidate & 0xFF000000u) != 0x02000000u || baseCandidate < 0x0200001Cu)
                continue;

            const u32 sseqRamOff = (baseCandidate - 0x1Cu) & mainRamMask;
            if (sseqRamOff + 0x20 > mainRamSize)
                continue;
            if (readU32(mainRam + sseqRamOff) != kMagicSSEQ
                || readU32(mainRam + sseqRamOff + 0x10) != kMagicDATA)
                continue;

            const u32 fileSize = readU32(mainRam + sseqRamOff + 0x08);
            const u32 dataOffset = readU32(mainRam + sseqRamOff + 0x18);
            if (dataOffset != 0x1Cu || fileSize <= 0x1Cu || fileSize > 0x80000u || sseqRamOff + fileSize > mainRamSize)
                continue;

            const u32 dataSize = fileSize - 0x1Cu;
            const u32 wNext = readU32(wram + (i + 1) * 4u);
            const u32 wPrev = readU32(wram + (i - 1) * 4u);

            u32 curPtr = 0;
            if (wNext >= baseCandidate && wNext < baseCandidate + dataSize)
                curPtr = wNext;
            else if (wPrev >= baseCandidate && wPrev < baseCandidate + dataSize)
                curPtr = wPrev;
            else
                continue;

            const u32 slotKey = wramIndexBase + i;
            const u64 nHash = mixWords64(
                readU32(wram + (i - 2) * 4u),
                readU32(wram + (i - 1) * 4u),
                readU32(wram + (i + 1) * 4u),
                readU32(wram + (i + 2) * 4u));

            WramTrackSlot* foundSlot = nullptr;
            for (WramTrackSlot& slot : wramTrackSlots)
            {
                if (slot.wramIndex == slotKey)
                {
                    foundSlot = &slot;
                    break;
                }
            }

            if (!foundSlot)
            {
                if (wramTrackSlots.size() < 96)
                {
                    wramTrackSlots.push_back(WramTrackSlot{
                        slotKey,
                        baseCandidate,
                        nHash,
                        0,
                        emulatorFrameCounter,
                        emulatorFrameCounter,
                        curPtr - baseCandidate
                    });
                }
            }
            else if (foundSlot->baseAddr != baseCandidate)
            {
                foundSlot->baseAddr = baseCandidate;
                foundSlot->neighborhoodHash = nHash;
                foundSlot->changeCount = 1; // New base pointer just written into this track slot!
                foundSlot->lastChangeFrame = emulatorFrameCounter;
                foundSlot->firstSeenFrame = emulatorFrameCounter;
                foundSlot->curOffset = curPtr - baseCandidate;
            }
            else
            {
                foundSlot->curOffset = curPtr - baseCandidate;
                if (foundSlot->neighborhoodHash != nHash)
                {
                    foundSlot->neighborhoodHash = nHash;
                    foundSlot->changeCount++;
                    foundSlot->lastChangeFrame = emulatorFrameCounter;
                }
            }
        }
    };

    scanWramRegion(nds.ARM7WRAM, nds.ARM7WRAMSize, 0);
    scanWramRegion(nds.SharedWRAM, nds.SharedWRAMSize, 0x10000u);

    // Prune stale WRAM slots
    wramTrackSlots.erase(
        std::remove_if(
            wramTrackSlots.begin(),
            wramTrackSlots.end(),
            [&](const WramTrackSlot& slot) {
                return (emulatorFrameCounter - slot.lastChangeFrame) > 60u;
            }),
        wramTrackSlots.end());

    // Group actively ticking tracks by SSEQ baseAddr in MainRAM
    struct ActiveBaseCandidate
    {
        u32 baseAddr = 0;
        u32 sseqRamOff = 0;
        u32 dataSize = 0;
        u32 tickingTracks = 0;
        u32 minFirstSeenFrame = 0xFFFFFFFFu;
    };
    std::vector<ActiveBaseCandidate> candidates;

    for (const WramTrackSlot& slot : wramTrackSlots)
    {
        if (slot.changeCount == 0 || (emulatorFrameCounter - slot.lastChangeFrame) > 18u)
            continue;

        const u32 sseqRamOff = (slot.baseAddr - 0x1Cu) & mainRamMask;
        if (sseqRamOff + 0x20 > mainRamSize || readU32(mainRam + sseqRamOff) != kMagicSSEQ)
            continue;
        const u32 fileSize = readU32(mainRam + sseqRamOff + 0x08);
        if (fileSize <= 0x1Cu || sseqRamOff + fileSize > mainRamSize)
            continue;

        ActiveBaseCandidate* cand = nullptr;
        for (auto& existing : candidates)
        {
            if (existing.baseAddr == slot.baseAddr)
            {
                cand = &existing;
                break;
            }
        }
        if (!cand)
        {
            candidates.push_back(ActiveBaseCandidate{
                slot.baseAddr,
                sseqRamOff,
                fileSize - 0x1Cu,
                1,
                slot.firstSeenFrame
            });
        }
        else
        {
            cand->tickingTracks++;
            cand->minFirstSeenFrame = std::min(cand->minFirstSeenFrame, slot.firstSeenFrame);
        }
    }

    // Score each candidate and match to SDAT database (or MainRAM fallback)
    int bestScore = -1;
    std::shared_ptr<PreparedSong> bestSong;
    u32 bestBaseAddr = 0;
    u32 bestFirstSeenFrame = emulatorFrameCounter;

    for (const ActiveBaseCandidate& cand : candidates)
    {
        const u8* seqBytes = mainRam + cand.sseqRamOff + 0x1Cu;
        const u64 seqHash = fnv1a64(seqBytes, std::min<u32>(cand.dataSize, 256u));

        const SdatSeqEntry* matchedEntry = nullptr;
        size_t matchedArchiveIdx = 0;

        for (size_t a = 0; a < sdatArchives.size() && !matchedEntry; ++a)
        {
            for (const SdatSeqEntry& seq : sdatArchives[a].sequences)
            {
                if (seq.dataSize == cand.dataSize && seq.bytecodeHash == seqHash)
                {
                    if (std::memcmp(
                            currentRomPtr + seq.romOffset + seq.dataOffset,
                            seqBytes,
                            std::min<u32>(cand.dataSize, 128u)) == 0)
                    {
                        matchedEntry = &seq;
                        matchedArchiveIdx = a;
                        break;
                    }
                }
            }
        }

        bool hasLoop = false;
        u8 ply = 0;
        if (matchedEntry)
        {
            hasLoop = matchedEntry->hasLoopJump;
            ply = matchedEntry->ply;
        }
        else
        {
            u32 dummyMinLoop = 0;
            std::vector<bool> dummyProgs;
            analyzeSseqBytecode(seqBytes, cand.dataSize, hasLoop, dummyMinLoop, dummyProgs);
        }

        // If our current looping BGM is still actively ticking in candidates, ignore short non-looping SFX sequences
        if (!hasLoop && cand.dataSize < 512 && activeBgmMainRamBase != 0 && cand.baseAddr != activeBgmMainRamBase)
        {
            bool currentBgmStillTicking = false;
            for (const ActiveBaseCandidate& other : candidates)
            {
                if (other.baseAddr == activeBgmMainRamBase)
                {
                    currentBgmStillTicking = true;
                    break;
                }
            }
            if (currentBgmStillTicking)
                continue;
        }

        int score = 0;
        if (hasLoop) score += 1000;
        if (ply == 0) score += 500;
        score += static_cast<int>(cand.tickingTracks) * 50;
        score += static_cast<int>(std::min<u32>(cand.dataSize / 16u, 200u));

        if (score > bestScore)
        {
            std::shared_ptr<PreparedSong> prepared;
            if (matchedEntry)
            {
                prepared = getOrBuildSdatSseqSong(matchedArchiveIdx, *matchedEntry, mainRam, mainRamSize, mainRamMask);
            }
            if (!prepared)
            {
                prepared = buildMainRamFallbackSong(
                    cand.sseqRamOff,
                    0x1Cu,
                    cand.dataSize,
                    mainRam,
                    mainRamSize,
                    mainRamMask,
                    nds.ARM7WRAM,
                    nds.ARM7WRAMSize);
            }
            if (prepared)
            {
                bestScore = score;
                bestSong = std::move(prepared);
                bestBaseAddr = cand.baseAddr;
                bestFirstSeenFrame = cand.minFirstSeenFrame;
            }
        }
    }

    if (bestSong)
    {
        recentRomReadSeqKey.store(0, std::memory_order_relaxed);
        if (activeBgmSongKey != bestSong->songKey)
        {
            activeBgmSongKey = bestSong->songKey;
            activeBgmMainRamBase = bestBaseAddr;
            activeBgmStartFrame = bestFirstSeenFrame;
            const u32 elapsed = (emulatorFrameCounter >= bestFirstSeenFrame)
                ? (emulatorFrameCounter - bestFirstSeenFrame)
                : 0u;
            activatePreparedSong(bestSong, elapsed);
        }
        return;
    }

    // Fallback: if no WRAM track slots were matched, check if a BGM SSEQ was just read from ROM
    const u32 romSeqKey = recentRomReadSeqKey.exchange(0, std::memory_order_relaxed);
    if (romSeqKey != 0 && wramTrackSlots.empty())
    {
        const size_t archiveIdx = static_cast<size_t>((romSeqKey >> 16) - 1u);
        const u32 seqId = romSeqKey & 0xFFFFu;
        if (archiveIdx < sdatArchives.size())
        {
            for (const SdatSeqEntry& seq : sdatArchives[archiveIdx].sequences)
            {
                if (seq.seqId == seqId)
                {
                    auto prepared = getOrBuildSdatSseqSong(archiveIdx, seq, mainRam, mainRamSize, mainRamMask);
                    if (prepared)
                    {
                        if (activeBgmSongKey != prepared->songKey)
                        {
                            activeBgmSongKey = prepared->songKey;
                            activeBgmMainRamBase = 0;
                            activeBgmStartFrame = emulatorFrameCounter;
                            activatePreparedSong(prepared, 0);
                        }
                        return;
                    }
                    break;
                }
            }
        }
    }

    // Check if a STRM stream is actively being read from ROM
    const u32 strmKey = recentRomReadStrmKey.exchange(0, std::memory_order_relaxed);
    if (strmKey != 0)
    {
        if (strmKey == lastStrmKeySeen && (emulatorFrameCounter - lastStrmSeenFrame) <= 60u)
            ++strmReadHitCount;
        else
            strmReadHitCount = 1;
        lastStrmKeySeen = strmKey;
        lastStrmSeenFrame = emulatorFrameCounter;

        if (strmReadHitCount >= 2)
        {
            const size_t archiveIdx = static_cast<size_t>((strmKey >> 16) - 1u);
            const u32 strmId = strmKey & 0xFFFFu;
            if (archiveIdx < sdatArchives.size())
            {
                for (const SdatStrmEntry& strm : sdatArchives[archiveIdx].streams)
                {
                    if (strm.strmId == strmId)
                    {
                        const u64 fullKey = (3ULL << 62) | (static_cast<u64>(archiveIdx) << 32) | strmId;
                        if (activeBgmSongKey != fullKey)
                        {
                            auto built = buildPreparedStrmSong(
                                fullKey,
                                currentRomPtr + strm.romOffset,
                                strm.romSize,
                                strm.vol);
                            if (built)
                            {
                                activeBgmSongKey = fullKey;
                                activeBgmMainRamBase = 0;
                                activeBgmStartFrame = emulatorFrameCounter;
                                activatePreparedSong(built, 0);
                            }
                        }
                        return;
                    }
                }
            }
        }
    }
    else if (currentBgmIsStream && (emulatorFrameCounter - lastStrmSeenFrame) <= 45u)
    {
        // Keep active STRM alive while between block reads
        return;
    }

    // If a ROM-read SSEQ fallback is active (activeBgmMainRamBase == 0 && !currentBgmIsStream),
    // keep it alive while BGM SPU channels are still playing on the hardware SPU.
    if (activeBgmSongKey != 0 && activeBgmMainRamBase == 0 && !currentBgmIsStream)
    {
        bool anyBgmChannelPlaying = false;
        for (u32 ch = 0; ch < 16; ++ch)
        {
            if ((nds.SPU.Read32(0x04000400u + ch * 0x10u) & (1u << 31)) != 0
                && channelIsBgm[ch].load(std::memory_order_relaxed))
            {
                anyBgmChannelPlaying = true;
                break;
            }
        }
        if (anyBgmChannelPlaying)
            return;
    }

    // If no active song has been seen, fade out
    if (activeBgmSongKey != 0)
    {
        deactivateCurrentSong();
    }
}

void onReset() noexcept
{
    Dse::onReset();
    wramTrackSlots.clear();
    activeBgmSongKey = 0;
    activeBgmMainRamBase = 0;
    hasActiveBgmForSpuFilter.store(false, std::memory_order_release);
    currentBgmSignatures.clear();
    reclassifyAllSpuChannels();

    std::lock_guard<std::mutex> lock(synthMutex);
    synthesizer.clearImmediate();
}

void onStateLoaded(melonDS::NDS& nds) noexcept
{
    (void)nds;
    Dse::onStateLoaded();
    wramTrackSlots.clear();
    activeBgmSongKey = 0;
    activeBgmMainRamBase = 0;
    hasActiveBgmForSpuFilter.store(false, std::memory_order_release);
    currentBgmSignatures.clear();
    reclassifyAllSpuChannels();

    std::lock_guard<std::mutex> lock(synthMutex);
    synthesizer.clearImmediate();
}

void onShutdown() noexcept
{
    onReset();
    Dse::onShutdown();
    currentRomPtr = nullptr;
    currentRomLength = 0;
    sdatArchives.clear();
    songCache.clear();
}

void onSpuChannelStart(
    melonDS::u32 channelNum,
    melonDS::u32 cnt,
    melonDS::u32 srcAddr,
    melonDS::u16 timerReload,
    melonDS::u32 loopPos,
    melonDS::u32 length,
    const melonDS::u32* fifo) noexcept
{
    (void)timerReload;
    if (channelNum >= 16 || !fifo)
        return;

    Dse::onSpuChannelStart(channelNum, srcAddr, loopPos, length);

    SpuChannelStartInfo info {};
    info.format = static_cast<u8>((cnt >> 29) & 0x3u);
    info.duty = static_cast<u8>((cnt >> 24) & 0x7u);
    info.loopPos = loopPos;
    info.length = length;
    info.fifo0 = fifo[0];
    info.fifo1 = fifo[1];

    channelStartInfos[channelNum] = info;
    const bool isBgm = classifySpuChannelAsBgm(channelNum, info);
    channelIsBgm[channelNum].store(isBgm, std::memory_order_relaxed);
}

void filterSpuSpeakerOutput(
    melonDS::u16 spuCnt,
    melonDS::u8 masterVolume,
    melonDS::u16 bias,
    bool applyBias,
    bool degrade10Bit,
    bool mute,
    melonDS::SPUChannel* channels,
    const melonDS::s32* channelSamples,
    melonDS::s16* output) noexcept
{
    if (!normalSpeedMusicEnabled.load(std::memory_order_relaxed) || !MelonDSAndroid::isFastForwardActive())
        return;

    if (!sfxEnabled.load(std::memory_order_relaxed))
    {
        output[0] = 0;
        output[1] = 0;
        return;
    }

    const bool dseBgmActive = Dse::hasActiveSongForSpuFilter();
    if (!hasActiveBgmForSpuFilter.load(std::memory_order_relaxed) && !dseBgmActive)
    {
        // No BGM active; let all SFX play through
        return;
    }

    if (!(spuCnt & (1u << 15)) || mute)
    {
        output[0] = 0;
        output[1] = 0;
        return;
    }

    s32 sfxLeft = 0;
    s32 sfxRight = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (channelIsBgm[i].load(std::memory_order_relaxed) || (dseBgmActive && Dse::isBgmChannel(i)))
            continue;
        if (i == 1 && (spuCnt & (1u << 12)))
            continue;
        if (i == 3 && (spuCnt & (1u << 13)))
            continue;
        channels[i].PanOutput(channelSamples[i], sfxLeft, sfxRight);
    }

    s32 leftOut = static_cast<s32>((static_cast<s64>(sfxLeft) * masterVolume) >> 15);
    s32 rightOut = static_cast<s32>((static_cast<s64>(sfxRight) * masterVolume) >> 15);

    if (applyBias)
    {
        leftOut += (static_cast<s32>(bias) << 6) - 0x8000;
        rightOut += (static_cast<s32>(bias) << 6) - 0x8000;
    }

    output[0] = static_cast<s16>(std::clamp<s32>(leftOut, -0x8000, 0x7FFF));
    output[1] = static_cast<s16>(std::clamp<s32>(rightOut, -0x8000, 0x7FFF));

    if (degrade10Bit)
    {
        output[0] &= static_cast<s16>(0xFFC0);
        output[1] &= static_cast<s16>(0xFFC0);
    }
}

bool processAudioOutput(
    melonDS::s16* audioData,
    int numFrames,
    bool fastForwardActive,
    bool muteOnFastForward) noexcept
{
    if (!normalSpeedMusicEnabled.load(std::memory_order_relaxed))
        return false;

    if (!fastForwardActive)
    {
        Dse::advanceSilentFrames(numFrames);
        std::lock_guard<std::mutex> lock(synthMutex);
        synthesizer.advanceSilentFrames(numFrames);
        return false;
    }

    const bool allowSfx = sfxEnabled.load(std::memory_order_relaxed);
    if (!allowSfx)
    {
        std::fill_n(audioData, numFrames * 2, 0);
    }

    if (Dse::renderIfActive(audioData, numFrames, allowSfx))
        return true;

    std::lock_guard<std::mutex> lock(synthMutex);
    if (synthesizer.hasActiveSong())
    {
        synthesizer.renderAndMix(audioData, numFrames, allowSfx);
    }
    else if (!allowSfx || muteOnFastForward)
    {
        std::fill_n(audioData, numFrames * 2, 0);
    }

    return true;
}

} // namespace MelonDSAndroid::FastForwardMusic
