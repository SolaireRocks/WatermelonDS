// Normal-speed music during fast-forward for games using Procyon Studio's
// Digital Sound Elements (DSE) sound engine.
//
// The file formats (smdl sequences, swdl wave banks), the event table, the envelope
// timing table and the pitch/volume rules follow dse-clef by Adam Higerd
// (https://github.com/ahigerd/dse-clef, MIT licence, copyright (c) 2020-2024 Adam Higerd).
// The in-RAM driver layout (each playing track keeps an absolute pointer to its current
// event inside the loaded smdl) comes from pmdsky-debug
// (https://github.com/UsernameFodder/pmdsky-debug).
//
// How it works:
//  1. At ROM load, every smdl/swdl file in the cartridge FAT is indexed.
//  2. While the game runs, main RAM is scanned for loaded smdl files. Each one is matched
//     to its ROM file by size and hash.
//  3. RAM is scanned for words that point inside each loaded smdl. Those that keep changing
//     are the driver's track read pointers, which tells us which song is actually playing
//     and where in the song the game is.
//  4. The playing song is rebuilt from the ROM (sequence + programs + samples from the
//     song's swdl and its main bank) and played by our own sequencer at 1x speed. When
//     fast-forward starts, the sequencer is re-synced to the game's track pointers.
//  5. SPU channels whose sample data matches the song's samples are treated as music so the
//     "sound effects during fast-forward" option can keep only the other channels.

#include "FastForwardMusicDse.h"
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

namespace MelonDSAndroid::FastForwardMusic::Dse
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
    constexpr int kMaxVoices = 32;
    constexpr int kMaxTracks = 32;
    constexpr int kFadeSamples = 480;
    constexpr float kMasterScale = 0.28f;

    constexpr u32 kMagicSmdl = 0x6C646D73u; // "smdl"
    constexpr u32 kMagicSwdl = 0x6C647773u; // "swdl"

    // Seconds are stored as milliseconds; index 127 means "forever".
    constexpr double kForever = -1.0;
    const double kEnvDurationMs[2][128] = {
        {
               0,    1,    2,    3,    4,    5,    6,    7,
               8,    9,   10,   11,   12,   13,   14,   15,
              16,   17,   18,   19,   20,   21,   22,   23,
              24,   25,   26,   27,   28,   29,   30,   31,
              32,   35,   40,   45,   51,   57,   64,   72,
              80,   88,   98,  109,  120,  131,  144,  158,
             172,  188,  204,  222,  240,  260,  281,  303,
             327,  352,  378,  406,  435,  466,  498,  532,
             568,  606,  645,  686,  729,  775,  822,  871,
             923,  977, 1030, 1090, 1150, 1220, 1280, 1350,
            1420, 1570, 1650, 1740, 1820, 1910, 2010, 2100,
            2200, 2310, 2410, 2520, 2640, 2750, 2880, 3000,
            3130, 3260, 3400, 3550, 3690, 3840, 4000, 4160,
            4330, 4500, 4670, 4850, 5040, 5230, 5430, 5630,
            5840, 6050, 6270, 6490, 6720, 6960, 7200, 7450,
            7710, 7970, 8240, 8520, 8800, 9090, 10000, kForever
        }, {
                 0,      4,      7,     10,     15,     21,     28,     36,
                46,     58,     72,     87,    104,    123,    145,    168,
               389,    446,    508,    575,    648,    726,    810,    901,
               997,   1100,   1210,   1326,   1449,   1580,   1717,   1862,
              3023,   3264,   3517,   3782,   4060,   4351,   4655,   4972,
              5302,   5647,   6005,   6378,   6765,   7167,   7584,   8017,
             11286,  11904,  12544,  13205,  13889,  14594,  15323,  16074,
             16848,  17646,  18468,  19315,  20185,  21081,  22002,  22948,
             29900,  31147,  32428,  33742,  35089,  36471,  37887,  39338,
             40824,  42346,  43904,  45499,  47130,  48798,  50503,  52247,
             64834,  67019,  69250,  71528,  73854,  76228,  78651,  81122,
             83643,  86213,  88834,  91506,  94229,  97003,  99829, 102707,
            123245, 126727, 130272, 133879, 137551, 141286, 145086, 148951,
            152882, 156879, 160942, 165072, 169270, 173536, 177870, 182274,
            213424, 218616, 223888, 229241, 234676, 240193, 245793, 251476,
            257242, 263093, 269029, 275050, 281157, 287351, 293632, kForever
        }
    };

    constexpr u32 kRestTicks[16] = { 96, 72, 64, 48, 36, 32, 24, 18, 16, 12, 9, 8, 6, 4, 3, 2 };

    // Parameter byte counts for events 0x90-0xFF; -1 = invalid event (the driver stops the track).
    constexpr s8 kEventParamLen[0x70] = {
        // 0x90
        0, 1, 1, 2, 3, 1, -1, -1, 0, 0, -1, -1, 1, 0, 0, -1,
        // 0xA0
        1, 1, -1, -1, 1, 1, -1, -1, 2, 1, 1, 1, 1, -1, -1, 3,
        // 0xB0
        0, 1, 1, 1, 2, 1, 1, -1, -1, -1, -1, -1, 1, -1, 1, 1,
        // 0xC0
        0, -1, -1, 1, -1, -1, -1, -1, -1, -1, -1, 2, -1, -1, -1, -1,
        // 0xD0
        1, 1, 1, 2, 3, 2, 2, 2, 2, -1, -1, 1, 5, 4, -1, 1,
        // 0xE0
        1, 1, 3, 1, 5, 4, -1, 1, 1, 1, 3, -1, 5, 4, -1, 1,
        // 0xF0
        5, 4, 2, 3, -1, -1, 1, -1, 2, -1, -1, -1, -1, -1, -1, -1,
    };

    inline u16 rd16(const u8* p) noexcept { return static_cast<u16>(p[0] | (p[1] << 8)); }
    inline u32 rd32(const u8* p) noexcept
    {
        return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8)
            | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
    }
    inline bool isMagic(const u8* p, const char* m) noexcept { return std::memcmp(p, m, 4) == 0; }

    u64 fnv1a64(const u8* data, size_t len) noexcept
    {
        u64 hash = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i)
        {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    inline float combinePan(float a, float b) noexcept
    {
        return std::clamp(a + b - 0.5f, 0.0f, 1.0f);
    }

    // ---------------------------------------------------------------------
    // Chunk walking (DSE files are a header followed by tagged chunks)
    // ---------------------------------------------------------------------

    struct ChunkRef
    {
        char magic[4] {};
        u32 offset = 0;     // chunk header start
        u32 dataOffset = 0; // offset + 0x10
        u32 dataEnd = 0;    // end of the chunk's declared data
    };

    bool isValidChunkMagic(const u8* p, const u8* end) noexcept
    {
        if (p + 8 > end)
            return false;
        for (int i = 0; i < 4; ++i)
        {
            if (p[i] >= 0x80 || (p[i] & 0x60) == 0)
                return false;
        }
        return p[4] == 0 && p[5] == 0;
    }

    std::vector<ChunkRef> walkChunks(const u8* file, u32 fileSize, u32 headerSize)
    {
        std::vector<ChunkRef> chunks;
        const u8* end = file + fileSize;
        u32 off = headerSize;
        while (off + 0x10 <= fileSize && chunks.size() < 512)
        {
            const u8* c = file + off;
            u32 size;
            u32 dataSize;
            if (isMagic(c, "song"))
            {
                size = 64;
                dataSize = 48;
            }
            else
            {
                dataSize = rd32(c + 0x0C);
                if (dataSize > fileSize)
                    break;
                size = dataSize + 0x10;
                if (size & 3u)
                    size = (size & ~3u) + 4u;
            }
            // Some files pad between chunks; skip ahead to the next valid tag.
            while (off + size + 8 <= fileSize && !isValidChunkMagic(file + off + size, end))
                size += 4;
            if (off + size > fileSize)
                size = fileSize - off;

            ChunkRef ref;
            std::memcpy(ref.magic, c, 4);
            ref.offset = off;
            ref.dataOffset = off + 0x10;
            ref.dataEnd = std::min<u32>(off + 0x10 + dataSize, fileSize);
            chunks.push_back(ref);

            if (isMagic(c, "eoc ") || isMagic(c, "eod "))
                break;
            if (size == 0)
                break;
            off += size;
        }
        return chunks;
    }

    const ChunkRef* findChunk(const std::vector<ChunkRef>& chunks, const char* magic) noexcept
    {
        for (const ChunkRef& c : chunks)
        {
            if (std::memcmp(c.magic, magic, 4) == 0)
                return &c;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // ROM index
    // ---------------------------------------------------------------------

    struct RomDseFile
    {
        bool isSmdl = false;
        u32 romOffset = 0;
        u32 size = 0;
        u16 id = 0;
        char name[17] {};
        u16 mainbankId = 0;   // swdl only
        u32 pcmdSize = 0;     // swdl only
    };

    // ---------------------------------------------------------------------
    // Prepared song
    // ---------------------------------------------------------------------

    struct WaviInfo
    {
        bool valid = false;
        s8 fineTune = 0;
        s8 coarseTune = 0;
        u8 rootKey = 60;
        u8 format = 0;
        bool loop = false;
        u32 sampleRate = 0;
        u32 sampleStart = 0;
        u32 loopStart = 0;  // words
        u32 loopLength = 0; // words
    };

    struct DseSample
    {
        std::vector<s16> pcm;
        bool loop = false;
        u32 loopStart = 0;
        double baseStep = 0.0; // source samples per output sample at the root key
        u8 rootKey = 60;
        s8 coarseTune = 0;
        s8 fineTune = 0;
    };

    struct SampleSignature
    {
        u64 hash = 0;
        u32 totalBytes = 0;
        bool trivial = false;
    };

    struct DseSplit
    {
        u8 lowKey = 0;
        u8 highKey = 127;
        u8 lowVel = 0;
        u8 highVel = 127;
        s8 fineTune = 0;
        s8 coarseTune = 0;
        s8 transpose = 0;
        u8 volume = 127;
        u8 pan = 64;
        u8 bendRange = 2;
        bool envelope = false;
        u8 envMult = 0;
        u8 attackLevel = 127;
        u8 attack = 0;
        u8 decay = 0;
        u8 sustain = 127;
        u8 hold = 0;
        u8 fade = 127;
        u8 release = 0;
        u16 waveIndex = 0;
        s32 sampleSlot = -1;
    };

    struct DseProgram
    {
        u8 gain = 127;
        u8 pan = 64;
        std::vector<DseSplit> splits;
    };

    struct DseTrackInfo
    {
        u32 start = 0; // first event (file offset)
        u32 end = 0;   // end of events (file offset)
        u8 channel = 0;
    };

    struct DseSong
    {
        int romIndex = -1;
        std::vector<u8> smdl;
        u16 ticksPerBeat = 48;
        std::vector<DseTrackInfo> tracks;
        std::array<s16, 256> programIndex {};
        std::vector<DseProgram> programs;
        std::vector<DseSample> samples;
        std::vector<SampleSignature> signatures;
    };

    SampleSignature makeSignature(const u8* data, u32 available, u32 totalBytes) noexcept
    {
        SampleSignature sig;
        const u32 n = std::min<u32>(available, 64u);
        sig.hash = fnv1a64(data, n);
        sig.totalBytes = totalBytes;
        bool allSame = true;
        for (u32 i = 1; i < n; ++i)
        {
            if (data[i] != data[0])
            {
                allSame = false;
                break;
            }
        }
        bool restSame = n > 5;
        for (u32 i = 5; i < n; ++i)
        {
            if (data[i] != data[4])
            {
                restSame = false;
                break;
            }
        }
        sig.trivial = allSame || restSame || n < 16;
        return sig;
    }

    bool signatureMatches(const SampleSignature& songSig, const SampleSignature& channelSig) noexcept
    {
        if (songSig.hash != channelSig.hash)
            return false;
        return songSig.totalBytes == channelSig.totalBytes || !songSig.trivial;
    }

    struct ParsedSwdl
    {
        const u8* file = nullptr;
        u32 size = 0;
        std::vector<WaviInfo> wavi; // indexed by wave number
        const u8* pcmd = nullptr;
        u32 pcmdSize = 0;
        const ChunkRef* prgi = nullptr;
        std::vector<ChunkRef> chunks;
        u16 numWaves = 0;
        u16 numPrograms = 0;
    };

    void parseWaviEntry(const u8* e, WaviInfo& info) noexcept
    {
        info.valid = true;
        info.fineTune = static_cast<s8>(e[0x04]);
        info.coarseTune = static_cast<s8>(e[0x05]);
        info.rootKey = e[0x06];
        info.format = e[0x13];
        info.loop = e[0x15] != 0;
        info.sampleRate = rd32(e + 0x20);
        info.sampleStart = rd32(e + 0x24);
        info.loopStart = rd32(e + 0x28);
        info.loopLength = rd32(e + 0x2C);
    }

    bool parseSwdl(const u8* file, u32 size, ParsedSwdl& out)
    {
        if (!file || size < 0x50 || rd32(file) != kMagicSwdl)
            return false;
        out.file = file;
        out.size = size;
        out.numWaves = rd16(file + 0x46);
        out.numPrograms = rd16(file + 0x48);
        out.chunks = walkChunks(file, size, 0x50);

        if (const ChunkRef* pcmd = findChunk(out.chunks, "pcmd"))
        {
            out.pcmd = file + pcmd->dataOffset;
            out.pcmdSize = pcmd->dataEnd - pcmd->dataOffset;
        }
        out.prgi = findChunk(out.chunks, "prgi");

        const ChunkRef* wavi = findChunk(out.chunks, "wavi");
        if (wavi && out.numWaves > 0)
        {
            const u32 slots = std::min<u32>(out.numWaves, 4096u);
            const u32 tableBase = wavi->dataOffset;
            out.wavi.assign(slots, WaviInfo{});

            // Preferred: the pointer table (offsets relative to the chunk data).
            u32 validFromTable = 0;
            for (u32 i = 0; i < slots && tableBase + i * 2 + 2 <= wavi->dataEnd; ++i)
            {
                const u16 ptr = rd16(file + tableBase + i * 2);
                if (ptr == 0)
                    continue;
                const u32 entry = tableBase + ptr;
                if (entry + 0x40 > wavi->dataEnd || rd16(file + entry + 2) != i)
                    continue;
                parseWaviEntry(file + entry, out.wavi[i]);
                ++validFromTable;
            }

            // Fallback: entries stored back to back after the table (dse-clef's approach).
            if (validFromTable == 0)
            {
                u32 off = 0x10 + slots * 2;
                if (off & 0x0Fu)
                    off = (off & ~0x0Fu) + 0x10u;
                off += wavi->offset;
                for (u32 i = 0; i < slots && off + 0x40 <= wavi->dataEnd; ++i, off += 0x40)
                {
                    const u16 index = rd16(file + off + 2);
                    if (index < slots && !out.wavi[index].valid)
                        parseWaviEntry(file + off, out.wavi[index]);
                }
            }
        }
        return true;
    }

    bool parseProgramAt(const u8* file, u32 entry, u32 chunkEnd, DseProgram& prog, u16& outId, u32& outNext)
    {
        if (entry + 0x10 > chunkEnd)
            return false;
        const u8* p = file + entry;
        outId = rd16(p);
        const u16 numSplits = rd16(p + 0x02);
        prog.gain = p[0x04];
        prog.pan = p[0x05];
        const u8 numLfos = p[0x0B];
        u32 off = entry + 0x10 + static_cast<u32>(numLfos) * 0x10u + 0x10u;
        if (numSplits > 128)
            return false;
        for (u32 s = 0; s < numSplits; ++s, off += 0x30)
        {
            if (off + 0x30 > chunkEnd)
                return false;
            const u8* sp = file + off;
            DseSplit split;
            split.bendRange = sp[0x02];
            split.lowKey = sp[0x04];
            split.highKey = sp[0x05];
            split.lowVel = sp[0x08];
            split.highVel = sp[0x09];
            split.waveIndex = rd16(sp + 0x12);
            split.fineTune = static_cast<s8>(sp[0x14]);
            split.coarseTune = static_cast<s8>(sp[0x15]);
            split.transpose = static_cast<s8>(sp[0x17]);
            split.volume = sp[0x18];
            split.pan = sp[0x19];
            split.envelope = sp[0x20] != 0;
            split.envMult = sp[0x21];
            split.attackLevel = sp[0x28];
            split.attack = std::min<u8>(sp[0x29], 127);
            split.decay = std::min<u8>(sp[0x2A], 127);
            split.sustain = sp[0x2B];
            split.hold = std::min<u8>(sp[0x2C], 127);
            split.fade = std::min<u8>(sp[0x2D], 127);
            split.release = std::min<u8>(sp[0x2E], 127);
            if (split.decay == 0 && split.sustain == 0)
                split.decay = 127; // decay disabled
            prog.splits.push_back(split);
        }
        outNext = off;
        return true;
    }

    void parsePrograms(const ParsedSwdl& swdl, DseSong& song)
    {
        song.programIndex.fill(-1);
        if (!swdl.prgi || swdl.numPrograms == 0)
            return;
        const u8* file = swdl.file;
        const ChunkRef& prgi = *swdl.prgi;
        const u32 slots = std::min<u32>(swdl.numPrograms, 256u);
        const u32 tableBase = prgi.dataOffset;

        auto addProgram = [&](u16 id, DseProgram&& prog) {
            if (id >= 256 || song.programIndex[id] >= 0)
                return;
            song.programIndex[id] = static_cast<s16>(song.programs.size());
            song.programs.push_back(std::move(prog));
        };

        u32 validFromTable = 0;
        for (u32 i = 0; i < slots && tableBase + i * 2 + 2 <= prgi.dataEnd; ++i)
        {
            const u16 ptr = rd16(file + tableBase + i * 2);
            if (ptr == 0)
                continue;
            DseProgram prog;
            u16 id = 0;
            u32 next = 0;
            if (!parseProgramAt(file, tableBase + ptr, prgi.dataEnd, prog, id, next) || id != i)
                continue;
            addProgram(id, std::move(prog));
            ++validFromTable;
        }

        if (validFromTable == 0)
        {
            u32 off = 0x10 + slots * 2;
            if (off & 0x0Fu)
                off = (off & ~0x0Fu) + 0x10u;
            off += prgi.offset;
            for (u32 i = 0; i < slots && off < prgi.dataEnd; ++i)
            {
                DseProgram prog;
                u16 id = 0;
                u32 next = 0;
                if (!parseProgramAt(file, off, prgi.dataEnd, prog, id, next))
                    break;
                addProgram(id, std::move(prog));
                off = next;
            }
        }
    }

    bool decodeSample(const u8* raw, u32 bytes, u8 format, const WaviInfo& phys, DseSample& out)
    {
        if (!raw || bytes == 0)
            return false;
        switch (format)
        {
        case 0: // PCM8
            out.pcm.resize(bytes);
            for (u32 i = 0; i < bytes; ++i)
                out.pcm[i] = static_cast<s16>(static_cast<s8>(raw[i]) * 256);
            out.loopStart = phys.loopStart * 4u;
            break;
        case 1: // PCM16
        {
            const u32 n = bytes / 2;
            if (n == 0)
                return false;
            out.pcm.resize(n);
            for (u32 i = 0; i < n; ++i)
                out.pcm[i] = static_cast<s16>(rd16(raw + i * 2));
            out.loopStart = phys.loopStart * 2u;
            break;
        }
        case 2: // IMA-ADPCM with a 4-byte header
        {
            if (bytes <= 4)
                return false;
            const u32 n = (bytes - 4) * 2;
            out.pcm.resize(n);
            s32 val = static_cast<s16>(rd16(raw));
            s32 idx = std::clamp<s32>(raw[2] & 0x7F, 0, 88);
            for (u32 b = 0; b < bytes - 4; ++b)
            {
                const u8 byteVal = raw[4 + b];
                for (int k = 0; k < 2; ++k)
                {
                    const u8 nib = k == 0 ? (byteVal & 0x0F) : (byteVal >> 4);
                    const u16 step = melonDS::SPUChannel::ADPCMTable[idx];
                    u16 diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    if (nib & 8)
                        val = std::max<s32>(val - diff, -0x7FFF);
                    else
                        val = std::min<s32>(val + diff, 0x7FFF);
                    idx = std::clamp<s32>(idx + melonDS::SPUChannel::ADPCMIndexTable[nib & 7], 0, 88);
                    out.pcm[b * 2 + k] = static_cast<s16>(val);
                }
            }
            out.loopStart = phys.loopStart >= 1 ? phys.loopStart * 8u - 8u : 0u;
            break;
        }
        default:
            return false;
        }
        out.loop = phys.loop && out.loopStart < out.pcm.size();
        return true;
    }

    std::shared_ptr<DseSong> buildSong(
        int romIndex,
        const u8* rom,
        const RomDseFile& smdlFile,
        const RomDseFile* swdlFile,
        const RomDseFile* bankFile)
    {
        auto song = std::make_shared<DseSong>();
        song->romIndex = romIndex;
        song->smdl.assign(rom + smdlFile.romOffset, rom + smdlFile.romOffset + smdlFile.size);
        const u8* smdl = song->smdl.data();
        const u32 smdlSize = static_cast<u32>(song->smdl.size());

        std::vector<ChunkRef> chunks = walkChunks(smdl, smdlSize, 0x40);
        if (const ChunkRef* songChunk = findChunk(chunks, "song"))
        {
            const u16 tpb = rd16(smdl + songChunk->offset + 0x12);
            if (tpb > 0 && tpb <= 960)
                song->ticksPerBeat = tpb;
        }
        for (const ChunkRef& c : chunks)
        {
            if (std::memcmp(c.magic, "trk ", 4) != 0 || c.offset + 0x14 > c.dataEnd)
                continue;
            DseTrackInfo info;
            info.channel = smdl[c.offset + 0x11] & 0x0Fu;
            info.start = c.offset + 0x14;
            info.end = c.dataEnd;
            song->tracks.push_back(info);
            if (song->tracks.size() >= kMaxTracks)
                break;
        }
        if (song->tracks.empty())
            return nullptr;

        ParsedSwdl swdl;
        ParsedSwdl bank;
        const bool hasSwdl = swdlFile && parseSwdl(rom + swdlFile->romOffset, swdlFile->size, swdl);
        const bool hasBank = bankFile && parseSwdl(rom + bankFile->romOffset, bankFile->size, bank);
        if (!hasSwdl)
            return nullptr;

        parsePrograms(swdl, *song);
        if (song->programs.empty())
            return nullptr;

        const ParsedSwdl& pcmSource = (hasBank && bank.pcmd) ? bank : swdl;
        if (!pcmSource.pcmd)
            return nullptr;

        std::vector<s32> slotForWave(65536, -1);
        for (DseProgram& prog : song->programs)
        {
            for (DseSplit& split : prog.splits)
            {
                const u16 wave = split.waveIndex;
                if (slotForWave[wave] != -1)
                {
                    split.sampleSlot = slotForWave[wave];
                    continue;
                }
                slotForWave[wave] = -2; // tried

                const WaviInfo* artic = (wave < swdl.wavi.size() && swdl.wavi[wave].valid) ? &swdl.wavi[wave] : nullptr;
                const WaviInfo* phys = (hasBank && wave < bank.wavi.size() && bank.wavi[wave].valid) ? &bank.wavi[wave] : nullptr;
                if (&pcmSource == &swdl)
                    phys = artic;
                if (!artic)
                    artic = phys;
                if (!phys)
                    phys = artic;
                if (!phys)
                    continue;

                const u64 bytes64 = 4ull * (static_cast<u64>(phys->loopStart) + phys->loopLength);
                if (bytes64 == 0 || static_cast<u64>(phys->sampleStart) + bytes64 > pcmSource.pcmdSize)
                    continue;
                const u32 bytes = static_cast<u32>(bytes64);
                const u8* raw = pcmSource.pcmd + phys->sampleStart;

                DseSample sample;
                if (!decodeSample(raw, bytes, phys->format, *phys, sample))
                    continue;
                const u32 rate = artic->sampleRate ? artic->sampleRate : phys->sampleRate;
                sample.baseStep = static_cast<double>(rate ? rate : 22050u) / kOutputSampleRate;
                sample.rootKey = artic->rootKey;
                sample.coarseTune = artic->coarseTune;
                sample.fineTune = artic->fineTune;

                const s32 slot = static_cast<s32>(song->samples.size());
                song->samples.push_back(std::move(sample));
                song->signatures.push_back(makeSignature(raw, bytes, bytes));
                slotForWave[wave] = slot;
                split.sampleSlot = slot;
            }
        }
        if (song->samples.empty())
            return nullptr;
        return song;
    }

    // ---------------------------------------------------------------------
    // Sequencer + synthesizer
    // ---------------------------------------------------------------------

    class DseSynth
    {
    public:
        void setSong(std::shared_ptr<const DseSong> newSong) noexcept
        {
            song = std::move(newSong);
            fadingOut = false;
            fadeOutRemaining = 0;
            fadeInRemaining = kFadeSamples;
            resetState();
        }

        void stop() noexcept
        {
            if (song && !fadingOut)
            {
                fadingOut = true;
                fadeOutRemaining = kFadeSamples;
            }
        }

        void clear() noexcept
        {
            song.reset();
            fadingOut = false;
            for (Voice& v : voices)
                v.active = false;
        }

        bool hasSong() const noexcept { return song != nullptr; }

        // Re-run the sequence silently until the track containing fileOffset reaches it.
        bool seekToFileOffset(u32 fileOffset) noexcept
        {
            if (!song)
                return false;
            int target = -1;
            for (size_t t = 0; t < song->tracks.size(); ++t)
            {
                if (fileOffset >= song->tracks[t].start && fileOffset < song->tracks[t].end)
                {
                    target = static_cast<int>(t);
                    break;
                }
            }
            if (target < 0)
                return false;

            resetState();
            u32 ticks = 0;
            while (tracks[target].active && tracks[target].pos < fileOffset && ticks < 200000u)
            {
                tick(true);
                ++ticks;
            }
            for (Voice& v : voices)
                v.active = false;
            tickPhase = 0.0;
            return tracks[target].pos >= fileOffset;
        }

        void advanceSilent(int numFrames) noexcept
        {
            if (!song || numFrames <= 0)
                return;
            if (fadingOut)
            {
                song.reset();
                fadingOut = false;
                return;
            }
            for (Voice& v : voices)
                v.active = false;
            tickPhase += numFrames;
            while (tickPhase >= samplesPerTick)
            {
                tickPhase -= samplesPerTick;
                tick(true);
            }
        }

        void render(s16* out, int numFrames, bool mixWithExisting) noexcept
        {
            if (!song)
                return;
            for (int f = 0; f < numFrames; ++f)
            {
                tickPhase += 1.0;
                while (song && tickPhase >= samplesPerTick)
                {
                    tickPhase -= samplesPerTick;
                    tick(false);
                }

                float mixL = 0.0f;
                float mixR = 0.0f;
                for (Voice& v : voices)
                {
                    if (!v.active)
                        continue;
                    const DseSample& smp = song->samples[v.sampleSlot];
                    const u32 total = static_cast<u32>(smp.pcm.size());
                    if (v.phase >= total)
                    {
                        if (smp.loop)
                        {
                            const double loopLen = static_cast<double>(total - smp.loopStart);
                            v.phase = smp.loopStart + std::fmod(v.phase - smp.loopStart, loopLen);
                        }
                        else
                        {
                            v.active = false;
                            continue;
                        }
                    }
                    const u32 i0 = static_cast<u32>(v.phase);
                    u32 i1 = i0 + 1;
                    if (i1 >= total)
                        i1 = smp.loop ? smp.loopStart : i0;
                    const float frac = static_cast<float>(v.phase - i0);
                    const float s = smp.pcm[i0] + (smp.pcm[i1] - smp.pcm[i0]) * frac;
                    v.phase += v.step;

                    const float env = stepEnvelope(v);
                    if (!v.active)
                        continue;
                    mixL += s * v.gainL * env;
                    mixR += s * v.gainR * env;
                }

                float scale = kMasterScale;
                if (fadeInRemaining > 0)
                {
                    scale *= static_cast<float>(kFadeSamples - fadeInRemaining) / kFadeSamples;
                    --fadeInRemaining;
                }
                if (fadingOut)
                {
                    if (fadeOutRemaining > 0)
                    {
                        scale *= static_cast<float>(fadeOutRemaining) / kFadeSamples;
                        --fadeOutRemaining;
                    }
                    else
                    {
                        clear();
                        scale = 0.0f;
                    }
                }

                s32 l = static_cast<s32>(mixL * scale);
                s32 r = static_cast<s32>(mixR * scale);
                if (mixWithExisting)
                {
                    l += out[f * 2];
                    r += out[f * 2 + 1];
                }
                out[f * 2] = static_cast<s16>(std::clamp<s32>(l, -32768, 32767));
                out[f * 2 + 1] = static_cast<s16>(std::clamp<s32>(r, -32768, 32767));

                if (!song)
                {
                    for (int g = f + 1; g < numFrames && !mixWithExisting; ++g)
                    {
                        out[g * 2] = 0;
                        out[g * 2 + 1] = 0;
                    }
                    return;
                }
            }
        }

    private:
        enum EnvPhase : u8 { EnvOff = 0, EnvAttack, EnvHold, EnvDecay, EnvSustain, EnvRelease };

        struct LoopFrame
        {
            u32 start = 0;
            u32 cont = 0;
            u8 left = 0;
            bool hasCont = false;
        };

        struct TrackState
        {
            bool active = false;
            bool polling = false;
            u32 pos = 0;
            u32 end = 0;
            s64 loopPos = -1;
            u32 wait = 0;
            int octave = 4;
            u32 lastRest = 0;
            u32 lastNote = 0;
            int program = -1;
            float volume = 100.0f / 127.0f;
            float expression = 1.0f;
            float pan = 0.5f;
            float bend = 0.0f;
            u8 bendRange = 0;
            u8 channel = 0;
            // Envelope overrides (-1 = use the split's value)
            bool envOverride = false;
            s16 ovAttackLevel = -1, ovAttack = -1, ovHold = -1, ovDecay = -1, ovSustain = -1, ovFade = -1, ovRelease = -1;
            std::array<LoopFrame, 4> loops {};
            int loopDepth = 0;
        };

        struct Voice
        {
            bool active = false;
            u8 track = 0;
            s32 sampleSlot = -1;
            double phase = 0.0;
            double step = 0.0;
            double baseStep = 0.0;
            float baseSemis = 0.0f;
            u8 splitBendRange = 2;
            float noteGain = 1.0f;
            float notePan = 0.5f;
            float gainL = 0.0f;
            float gainR = 0.0f;
            s32 ticksLeft = -1;
            u64 birth = 0;

            u8 envPhase = EnvOff;
            float envLevel = 0.0f;
            float envDelta = 0.0f;
            s64 envRemain = -1;
            float attackLevel = 1.0f;
            s64 attackSamples = 0;
            s64 holdSamples = 0;
            s64 decaySamples = -1;
            float sustainLevel = 1.0f;
            s64 fadeSamples = -1;
            s64 releaseSamples = 0;
        };

        std::shared_ptr<const DseSong> song;
        std::array<TrackState, kMaxTracks> tracks {};
        std::array<Voice, kMaxVoices> voices {};
        std::array<float, 16> channelVolume {};
        std::array<float, 16> channelPan {};
        u32 tempo = 120;
        double samplesPerTick = 1.0;
        double tickPhase = 0.0;
        u64 voiceCounter = 0;
        bool fadingOut = false;
        int fadeInRemaining = 0;
        int fadeOutRemaining = 0;

        void updateTempo() noexcept
        {
            const double tpb = song ? std::max<u16>(song->ticksPerBeat, 1) : 48;
            samplesPerTick = (kOutputSampleRate * 60.0) / (tpb * std::max<u32>(tempo, 1));
        }

        void resetState() noexcept
        {
            for (TrackState& t : tracks)
                t = TrackState{};
            for (Voice& v : voices)
                v = Voice{};
            channelVolume.fill(1.0f);
            channelPan.fill(0.5f);
            tempo = 120;
            tickPhase = 0.0;
            if (song)
            {
                for (size_t i = 0; i < song->tracks.size() && i < tracks.size(); ++i)
                {
                    TrackState& t = tracks[i];
                    t.active = true;
                    t.pos = song->tracks[i].start;
                    t.end = song->tracks[i].end;
                    t.channel = song->tracks[i].channel;
                }
            }
            updateTempo();
        }

        static s64 envSamples(u8 index, u8 mult) noexcept
        {
            const int table = mult ? 0 : 1;
            const double ms = kEnvDurationMs[table][std::min<u8>(index, 127)];
            if (ms < 0.0)
                return -1;
            return static_cast<s64>(ms * (mult ? mult : 1) * 0.001 * kOutputSampleRate);
        }

        void enterEnvPhase(Voice& v, u8 phase) noexcept
        {
            v.envPhase = phase;
            v.envDelta = 0.0f;
            v.envRemain = -1;
            switch (phase)
            {
            case EnvAttack:
                v.envLevel = v.attackLevel;
                if (v.attackSamples <= 0)
                {
                    v.envLevel = 1.0f;
                    enterEnvPhase(v, EnvHold);
                    return;
                }
                v.envDelta = (1.0f - v.envLevel) / v.attackSamples;
                v.envRemain = v.attackSamples;
                return;
            case EnvHold:
                if (v.holdSamples == 0)
                {
                    enterEnvPhase(v, EnvDecay);
                    return;
                }
                v.envRemain = v.holdSamples; // -1 = hold forever
                return;
            case EnvDecay:
                if (v.decaySamples == 0)
                {
                    v.envLevel = v.sustainLevel;
                    enterEnvPhase(v, EnvSustain);
                    return;
                }
                if (v.decaySamples > 0)
                {
                    v.envDelta = (v.sustainLevel - v.envLevel) / v.decaySamples;
                    v.envRemain = v.decaySamples;
                }
                return;
            case EnvSustain:
                if (v.fadeSamples == 0)
                {
                    v.active = false;
                    return;
                }
                if (v.fadeSamples > 0)
                {
                    v.envDelta = -v.envLevel / v.fadeSamples;
                    v.envRemain = v.fadeSamples;
                }
                return;
            case EnvRelease:
                if (v.releaseSamples <= 0)
                {
                    v.active = false;
                    return;
                }
                v.envDelta = -v.envLevel / v.releaseSamples;
                v.envRemain = v.releaseSamples;
                return;
            default:
                v.active = false;
                return;
            }
        }

        float stepEnvelope(Voice& v) noexcept
        {
            const float level = v.envLevel;
            if (v.envRemain > 0)
            {
                v.envLevel = std::clamp(v.envLevel + v.envDelta, 0.0f, 1.0f);
                if (--v.envRemain == 0)
                {
                    switch (v.envPhase)
                    {
                    case EnvAttack: v.envLevel = 1.0f; enterEnvPhase(v, EnvHold); break;
                    case EnvHold: enterEnvPhase(v, EnvDecay); break;
                    case EnvDecay: v.envLevel = v.sustainLevel; enterEnvPhase(v, EnvSustain); break;
                    case EnvSustain:
                    case EnvRelease:
                    default: v.active = false; break;
                    }
                }
            }
            return level;
        }

        void releaseVoice(Voice& v) noexcept
        {
            if (v.active && v.envPhase != EnvRelease)
                enterEnvPhase(v, EnvRelease);
        }

        void updateVoice(Voice& v) noexcept
        {
            const TrackState& t = tracks[v.track];
            const float gain = v.noteGain * t.volume * t.expression * channelVolume[t.channel & 15];
            const float pan = combinePan(v.notePan, combinePan(t.pan, channelPan[t.channel & 15]));
            v.gainL = gain * std::min(1.0f, 2.0f * (1.0f - pan));
            v.gainR = gain * std::min(1.0f, 2.0f * pan);
            const float range = t.bendRange ? t.bendRange : v.splitBendRange;
            const float semis = v.baseSemis + t.bend * range;
            v.step = v.baseStep * std::exp2(semis / 12.0f);
        }

        bool trackHasHeldNotes(int trackIndex) const noexcept
        {
            for (const Voice& v : voices)
            {
                if (v.active && v.track == trackIndex && v.envPhase != EnvRelease)
                    return true;
            }
            return false;
        }

        void noteOn(int trackIndex, int key, u8 velocity, u32 duration) noexcept
        {
            TrackState& t = tracks[trackIndex];
            if (t.program < 0)
                return;
            const DseProgram& prog = song->programs[t.program];
            const DseSplit* split = nullptr;
            for (const DseSplit& s : prog.splits)
            {
                if (key >= s.lowKey && key <= s.highKey && velocity >= s.lowVel && velocity <= s.highVel)
                {
                    split = &s;
                    break;
                }
            }
            if (!split || split->sampleSlot < 0)
                return;
            const DseSample& smp = song->samples[split->sampleSlot];

            Voice* voice = nullptr;
            for (Voice& v : voices)
            {
                if (!v.active)
                {
                    voice = &v;
                    break;
                }
            }
            if (!voice)
            {
                voice = &voices[0];
                for (Voice& v : voices)
                {
                    const bool vRel = v.envPhase == EnvRelease;
                    const bool bestRel = voice->envPhase == EnvRelease;
                    if ((vRel && !bestRel) || (vRel == bestRel && v.birth < voice->birth))
                        voice = &v;
                }
            }

            Voice& v = *voice;
            v = Voice{};
            v.active = true;
            v.track = static_cast<u8>(trackIndex);
            v.sampleSlot = split->sampleSlot;
            v.phase = 0.0;
            v.baseStep = smp.baseStep;
            v.baseSemis = static_cast<float>(key - smp.rootKey) + split->transpose
                + (split->coarseTune - smp.coarseTune)
                + (split->fineTune - smp.fineTune) / 255.0f;
            v.splitBendRange = split->bendRange;
            v.noteGain = (prog.gain / 127.0f) * (split->volume / 127.0f) * (velocity / 127.0f);
            v.notePan = combinePan(prog.pan / 128.0f, split->pan / 128.0f);
            v.ticksLeft = static_cast<s32>(std::min<u32>(duration, 0x7FFFFFFFu));
            v.birth = ++voiceCounter;

            const bool useEnvelope = split->envelope || t.envOverride;
            if (useEnvelope)
            {
                auto pick = [](s16 ov, u8 base) -> u8 { return ov >= 0 ? static_cast<u8>(ov) : base; };
                const u8 mult = split->envMult;
                v.attackLevel = std::min(1.0f, pick(t.ovAttackLevel, split->attackLevel) / 127.0f);
                v.attackSamples = envSamples(pick(t.ovAttack, split->attack), mult);
                v.holdSamples = envSamples(pick(t.ovHold, split->hold), mult);
                v.decaySamples = envSamples(pick(t.ovDecay, split->decay), mult);
                v.sustainLevel = std::min(1.0f, pick(t.ovSustain, split->sustain) / 127.0f);
                v.fadeSamples = envSamples(pick(t.ovFade, split->fade), mult);
                v.releaseSamples = envSamples(pick(t.ovRelease, split->release), mult);
                if (v.attackSamples < 0)
                    v.attackSamples = 0;
                if (v.releaseSamples < 0)
                    v.releaseSamples = static_cast<s64>(10.0 * kOutputSampleRate);
            }
            else
            {
                v.attackLevel = 1.0f;
                v.attackSamples = 0;
                v.holdSamples = -1;
                v.decaySamples = -1;
                v.sustainLevel = 1.0f;
                v.fadeSamples = -1;
                v.releaseSamples = static_cast<s64>(0.08 * kOutputSampleRate);
            }
            enterEnvPhase(v, EnvAttack);
            if (v.active)
                updateVoice(v);
        }

        void tick(bool silent) noexcept
        {
            if (!song)
                return;
            if (!silent)
            {
                // Count down notes started on earlier ticks before new ones are played.
                for (Voice& v : voices)
                {
                    if (v.active && v.ticksLeft > 0 && --v.ticksLeft == 0)
                        releaseVoice(v);
                }
            }
            const size_t numTracks = std::min(song->tracks.size(), tracks.size());
            for (size_t i = 0; i < numTracks; ++i)
            {
                if (tracks[i].active)
                    stepTrack(static_cast<int>(i), silent);
            }
            if (silent)
                return;
            for (Voice& v : voices)
            {
                if (v.active)
                    updateVoice(v);
            }
        }

        void stepTrack(int index, bool silent) noexcept
        {
            TrackState& t = tracks[index];
            const u8* d = song->smdl.data();

            if (t.polling)
            {
                if (!silent && trackHasHeldNotes(index))
                    return;
                t.polling = false;
            }
            if (t.wait > 0 && --t.wait > 0)
                return;

            int ops = 0;
            while (t.active && t.wait == 0 && !t.polling && ++ops < 1024)
            {
                if (t.pos >= t.end)
                {
                    if (t.loopPos >= 0 && static_cast<u32>(t.loopPos) < t.end)
                        t.pos = static_cast<u32>(t.loopPos);
                    else
                        t.active = false;
                    continue;
                }

                const u8 op = d[t.pos++];
                if (op < 0x80) // Play note
                {
                    if (t.pos >= t.end)
                    {
                        t.active = false;
                        break;
                    }
                    const u8 p = d[t.pos++];
                    const int numBytes = p >> 6;
                    u32 duration = t.lastNote;
                    if (numBytes > 0)
                    {
                        if (t.pos + numBytes > t.end)
                        {
                            t.active = false;
                            break;
                        }
                        duration = 0;
                        for (int b = 0; b < numBytes; ++b)
                            duration = (duration << 8) | d[t.pos++];
                    }
                    t.lastNote = duration;
                    t.octave += ((p >> 4) & 3) - 2;
                    const int key = t.octave * 12 + (p & 0x0F);
                    if (!silent && duration > 0 && key >= 0 && key < 128)
                        noteOn(index, key, op, duration);
                    continue;
                }
                if (op < 0x90) // Fixed-length rest
                {
                    t.lastRest = kRestTicks[op - 0x80];
                    t.wait = t.lastRest;
                    continue;
                }

                const s8 len = kEventParamLen[op - 0x90];
                if (len < 0 || t.pos + static_cast<u32>(len) > t.end)
                {
                    t.active = false;
                    break;
                }
                const u8* a = d + t.pos;
                t.pos += static_cast<u32>(len);

                switch (op)
                {
                case 0x90: t.wait = t.lastRest; break;
                case 0x91: t.lastRest += a[0]; t.wait = t.lastRest; break;
                case 0x92: t.lastRest = a[0]; t.wait = t.lastRest; break;
                case 0x93: t.lastRest = rd16(a); t.wait = t.lastRest; break;
                case 0x94: t.lastRest = a[0] | (a[1] << 8) | (a[2] << 16); t.wait = t.lastRest; break;
                case 0x95: t.polling = true; break;
                case 0x98:
                    if (t.loopPos >= 0 && static_cast<u32>(t.loopPos) < t.end)
                        t.pos = static_cast<u32>(t.loopPos);
                    else
                        t.active = false;
                    break;
                case 0x99: t.loopPos = t.pos; break;
                case 0x9C:
                    if (t.loopDepth < 4)
                        t.loops[t.loopDepth++] = LoopFrame{t.pos, 0, a[0], false};
                    break;
                case 0x9D:
                    if (t.loopDepth > 0)
                    {
                        LoopFrame& lf = t.loops[t.loopDepth - 1];
                        lf.cont = t.pos;
                        lf.hasCont = true;
                        if (lf.left == 0)
                            t.pos = lf.start;
                        else if (--lf.left > 0)
                            t.pos = lf.start;
                        else
                            --t.loopDepth;
                    }
                    break;
                case 0x9E:
                    if (t.loopDepth > 0)
                    {
                        LoopFrame& lf = t.loops[t.loopDepth - 1];
                        if (lf.hasCont && lf.left == 1)
                        {
                            t.pos = lf.cont;
                            --t.loopDepth;
                        }
                    }
                    break;
                case 0xA0: t.octave = a[0]; break;
                case 0xA1: t.octave += static_cast<s8>(a[0]); break;
                case 0xA4:
                case 0xA5:
                    tempo = std::max<u32>(a[0], 1);
                    updateTempo();
                    break;
                case 0xAC:
                    t.program = song->programIndex[a[0]];
                    break;
                case 0xB0:
                    t.envOverride = false;
                    t.ovAttackLevel = t.ovAttack = t.ovHold = t.ovDecay = t.ovSustain = t.ovFade = t.ovRelease = -1;
                    break;
                case 0xB1: t.envOverride = true; t.ovAttackLevel = a[0]; break;
                case 0xB2: t.envOverride = true; t.ovAttack = std::min<u8>(a[0], 127); break;
                case 0xB3: t.envOverride = true; t.ovHold = std::min<u8>(a[0], 127); break;
                case 0xB4:
                    t.envOverride = true;
                    if (a[0] != 0xFF) t.ovDecay = std::min<u8>(a[0], 127);
                    if (a[1] != 0xFF) t.ovSustain = a[1];
                    break;
                case 0xB5: t.envOverride = true; t.ovFade = std::min<u8>(a[0], 127); break;
                case 0xB6: t.envOverride = true; t.ovRelease = std::min<u8>(a[0], 127); break;
                case 0xBE: channelPan[t.channel & 15] = a[0] / 128.0f; break;
                case 0xC3: channelVolume[t.channel & 15] = a[0] / 127.0f; break;
                case 0xD7: t.bend = static_cast<s16>((a[0] << 8) | a[1]) / 8192.0f; break;
                case 0xDB: t.bendRange = a[0]; break;
                case 0xE0: t.volume = a[0] / 127.0f; break;
                case 0xE1: t.volume = std::clamp(t.volume + static_cast<s8>(a[0]) / 127.0f, 0.0f, 1.0f); break;
                case 0xE3: t.expression = a[0] / 127.0f; break;
                case 0xE8: t.pan = a[0] / 128.0f; break;
                case 0xE9: t.pan = std::clamp(t.pan + static_cast<s8>(a[0]) / 128.0f, 0.0f, 1.0f); break;
                default: break; // Known but unsupported (sweeps, LFOs, tuning, no-ops)
                }
            }
        }
    };

    // ---------------------------------------------------------------------
    // Emulator-thread detection state
    // ---------------------------------------------------------------------

    struct PointerLoc
    {
        u8 region = 0; // 0 = main RAM, 1 = ARM7 WRAM, 2 = shared WRAM
        u32 offset = 0;
        u32 value = 0;
    };

    struct Resident
    {
        u32 ramOff = 0;
        u32 size = 0;
        int romIndex = -1;
        u32 firstSeenFrame = 0;
        bool seenThisScan = false;
        bool unplayable = false;
        std::vector<PointerLoc> pointers;
        bool active = false;
        u32 activeSince = 0;
        u32 lastActivityFrame = 0;
        u32 lastChangedValue = 0;
    };

    struct ChannelSignature
    {
        bool valid = false;
        SampleSignature sig;
    };

    constexpr u32 kActivityTimeoutFrames = 150;

    const u8* romPtr = nullptr;
    u32 romLength = 0;
    std::vector<RomDseFile> romFiles;
    std::atomic<bool> dseGame {false};

    std::vector<Resident> residents;
    std::vector<std::shared_ptr<DseSong>> songCache;
    std::shared_ptr<const DseSong> activeSong; // emulator thread copy (SPU classification)
    s64 activeResidentRamOff = -1;
    bool pointersEverSeen = false;
    bool residentsChanged = false;
    bool wasFastForward = false;

    u32 frameCounter = 0;
    u32 lastMagicScanFrame = 0;
    u32 lastPointerScanFrame = 0;

    const u8* mainRam = nullptr;
    u32 mainRamMask = 0;
    const u8* arm7Wram = nullptr;
    u32 arm7WramSize = 0;
    const u8* sharedWram = nullptr;
    u32 sharedWramSize = 0;

    std::atomic<bool> activeForSpuFilter {false};
    std::array<ChannelSignature, 16> channelSignatures {};
    std::array<std::atomic<bool>, 16> channelIsBgm {};

    std::mutex synthMutex;
    DseSynth synth;

    bool channelMatchesSong(const ChannelSignature& ch) noexcept
    {
        if (!ch.valid || !activeSong)
            return false;
        for (const SampleSignature& s : activeSong->signatures)
        {
            if (signatureMatches(s, ch.sig))
                return true;
        }
        return false;
    }

    void reclassifyChannels() noexcept
    {
        for (u32 ch = 0; ch < 16; ++ch)
            channelIsBgm[ch].store(channelMatchesSong(channelSignatures[ch]), std::memory_order_relaxed);
    }

    void clearDetection() noexcept
    {
        residents.clear();
        activeSong.reset();
        activeResidentRamOff = -1;
        residentsChanged = false;
        activeForSpuFilter.store(false, std::memory_order_release);
        for (auto& f : channelIsBgm)
            f.store(false, std::memory_order_relaxed);
        for (auto& s : channelSignatures)
            s = ChannelSignature{};
    }

    const u8* regionBase(u8 region, u32& size) noexcept
    {
        switch (region)
        {
        case 0: size = mainRamMask + 1; return mainRam;
        case 1: size = arm7WramSize; return arm7Wram;
        default: size = sharedWramSize; return sharedWram;
        }
    }

    bool valueInResident(u32 value, const Resident& r) noexcept
    {
        if ((value & 0xFF000000u) != 0x02000000u)
            return false;
        const u32 off = value & mainRamMask;
        return off >= r.ramOff + 0x40 && off < r.ramOff + r.size;
    }

    // Compares a few spans of the song body. The header isn't used because the driver may
    // write to it after loading.
    int countMatchingProbes(const RomDseFile& f, const u8* ram, u32 size) noexcept
    {
        int matches = 0;
        const u32 probes[3] = { size / 2, (size / 4) * 3, size > 0x40 ? size - 0x40 : 0 };
        for (u32 at : probes)
        {
            at &= ~3u;
            if (at < 0x90 || at + 0x20 > size)
            {
                ++matches; // too small to probe here; treat as matching
                continue;
            }
            if (std::memcmp(romPtr + f.romOffset + at, ram + at, 0x20) == 0)
                ++matches;
        }
        return matches;
    }

    int findRomSmdl(const u8* ram, u32 size) noexcept
    {
        int byName = -1;
        int byContent = -1;
        for (size_t i = 0; i < romFiles.size(); ++i)
        {
            const RomDseFile& f = romFiles[i];
            if (!f.isSmdl || f.size != size)
                continue;
            const int probes = countMatchingProbes(f, ram, size);
            const bool sameName = std::memcmp(f.name, ram + 0x20, 16) == 0;
            if (sameName && probes >= 2 && byName < 0)
                byName = static_cast<int>(i);
            else if (probes == 3 && byContent < 0)
                byContent = static_cast<int>(i);
        }
        return byName >= 0 ? byName : byContent;
    }

    void scanForLoadedSongs() noexcept
    {
        const u32 ramSize = mainRamMask + 1;
        for (Resident& r : residents)
            r.seenThisScan = false;

        for (u32 off = 0; off + 0x80 <= ramSize; off += 4)
        {
            if (rd32(mainRam + off) != kMagicSmdl)
                continue;
            const u8* h = mainRam + off;
            const u32 size = rd32(h + 8);
            if (size < 0x80 || size > 0x100000 || off + size > ramSize || !isMagic(h + 0x40, "song"))
                continue;

            Resident* existing = nullptr;
            for (Resident& r : residents)
            {
                if (r.ramOff == off)
                {
                    existing = &r;
                    break;
                }
            }

            const int romIndex = findRomSmdl(h, size);
            if (existing && existing->size == size && existing->romIndex == romIndex)
            {
                existing->seenThisScan = true;
                continue;
            }
            if (romIndex < 0)
            {
                if (existing)
                    existing->seenThisScan = false; // different / unknown content now
                continue;
            }
            if (existing)
            {
                *existing = Resident{};
                existing->ramOff = off;
                existing->size = size;
                existing->romIndex = romIndex;
                existing->firstSeenFrame = frameCounter;
                existing->seenThisScan = true;
            }
            else if (residents.size() < 16)
            {
                Resident r;
                r.ramOff = off;
                r.size = size;
                r.romIndex = romIndex;
                r.firstSeenFrame = frameCounter;
                r.seenThisScan = true;
                residents.push_back(std::move(r));
            }
            residentsChanged = true;
        }

        const size_t before = residents.size();
        residents.erase(
            std::remove_if(residents.begin(), residents.end(), [](const Resident& r) { return !r.seenThisScan; }),
            residents.end());
        if (residents.size() != before)
            residentsChanged = true;
    }

    void scanForTrackPointers() noexcept
    {
        for (Resident& r : residents)
            r.pointers.clear();
        if (residents.empty())
            return;

        for (u8 region = 0; region < 3; ++region)
        {
            u32 size = 0;
            const u8* base = regionBase(region, size);
            if (!base || size < 4)
                continue;
            for (u32 off = 0; off + 4 <= size; off += 4)
            {
                const u32 v = rd32(base + off);
                if ((v & 0xFF000000u) != 0x02000000u)
                    continue;
                for (Resident& r : residents)
                {
                    if (!valueInResident(v, r))
                        continue;
                    // Skip words inside the song file itself.
                    if (region == 0 && off >= r.ramOff && off < r.ramOff + r.size)
                        break;
                    if (r.pointers.size() < 128)
                        r.pointers.push_back(PointerLoc{region, off, v});
                    break;
                }
            }
        }
    }

    void updateActivity() noexcept
    {
        for (Resident& r : residents)
        {
            bool changed = false;
            for (PointerLoc& p : r.pointers)
            {
                u32 size = 0;
                const u8* base = regionBase(p.region, size);
                if (!base || p.offset + 4 > size)
                    continue;
                const u32 v = rd32(base + p.offset);
                if (v == p.value)
                    continue;
                p.value = v;
                if (valueInResident(v, r))
                {
                    changed = true;
                    r.lastChangedValue = v;
                }
            }
            r.pointers.erase(
                std::remove_if(r.pointers.begin(), r.pointers.end(), [&](const PointerLoc& p) { return !valueInResident(p.value, r); }),
                r.pointers.end());

            if (changed)
            {
                pointersEverSeen = true;
                if (!r.active)
                {
                    r.active = true;
                    r.activeSince = frameCounter;
                }
                r.lastActivityFrame = frameCounter;
            }
            else if (r.active && frameCounter - r.lastActivityFrame > kActivityTimeoutFrames)
            {
                r.active = false;
            }
        }
    }

    std::shared_ptr<DseSong> getOrBuildSong(int romIndex)
    {
        for (const auto& cached : songCache)
        {
            if (cached->romIndex == romIndex)
                return cached;
        }
        const RomDseFile& smdl = romFiles[romIndex];

        // Paired wave bank: same id, preferably the same internal name stem, nearest in the ROM.
        auto stem = [](const char* name) {
            size_t n = 0;
            while (n < 16 && name[n] && name[n] != '.')
                ++n;
            return n;
        };
        const RomDseFile* swdl = nullptr;
        int bestScore = -1;
        u64 bestDistance = ~0ull;
        for (const RomDseFile& f : romFiles)
        {
            if (f.isSmdl || f.id != smdl.id)
                continue;
            const size_t sa = stem(smdl.name);
            const bool sameName = sa > 0 && sa == stem(f.name) && std::memcmp(smdl.name, f.name, sa) == 0;
            const int score = sameName ? 1 : 0;
            const u64 distance = f.romOffset > smdl.romOffset ? f.romOffset - smdl.romOffset : smdl.romOffset - f.romOffset;
            if (score > bestScore || (score == bestScore && distance < bestDistance))
            {
                swdl = &f;
                bestScore = score;
                bestDistance = distance;
            }
        }
        if (!swdl)
            return nullptr;

        // Main bank with the samples, unless the paired file has its own.
        const RomDseFile* bank = nullptr;
        if (swdl->pcmdSize == 0)
        {
            bestDistance = ~0ull;
            for (const RomDseFile& f : romFiles)
            {
                if (f.isSmdl || f.pcmdSize == 0 || f.id != swdl->mainbankId)
                    continue;
                const u64 distance = f.romOffset > swdl->romOffset ? f.romOffset - swdl->romOffset : swdl->romOffset - f.romOffset;
                if (distance < bestDistance)
                {
                    bank = &f;
                    bestDistance = distance;
                }
            }
            if (!bank)
            {
                for (const RomDseFile& f : romFiles)
                {
                    if (!f.isSmdl && (!bank || f.pcmdSize > bank->pcmdSize))
                        bank = &f;
                }
            }
        }

        auto song = buildSong(romIndex, romPtr, smdl, swdl, bank);
        if (song)
        {
            if (songCache.size() >= 6)
                songCache.erase(songCache.begin());
            songCache.push_back(song);
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Info,
                "FastForwardMusic: DSE song %s (%zu tracks, %zu programs, %zu samples)\n",
                smdl.name, song->tracks.size(), song->programs.size(), song->samples.size());
        }
        else
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "FastForwardMusic: could not prepare DSE song %s\n",
                smdl.name);
        }
        return song;
    }

    void stopActiveSong() noexcept
    {
        if (activeResidentRamOff < 0 && !activeSong)
            return;
        activeResidentRamOff = -1;
        activeSong.reset();
        activeForSpuFilter.store(false, std::memory_order_release);
        reclassifyChannels();
        std::lock_guard<std::mutex> lock(synthMutex);
        synth.stop();
    }

    void seekToGame(const Resident& r) noexcept
    {
        if (r.lastChangedValue == 0)
            return;
        const u32 fileOffset = (r.lastChangedValue & mainRamMask) - r.ramOff;
        std::lock_guard<std::mutex> lock(synthMutex);
        synth.seekToFileOffset(fileOffset);
    }

    void activateResident(Resident& r) noexcept
    {
        if (activeSong && activeSong->romIndex == r.romIndex)
        {
            activeResidentRamOff = r.ramOff;
            return;
        }
        auto song = getOrBuildSong(r.romIndex);
        if (!song)
        {
            r.unplayable = true;
            stopActiveSong();
            return;
        }
        activeResidentRamOff = r.ramOff;
        activeSong = song;
        activeForSpuFilter.store(true, std::memory_order_release);
        reclassifyChannels();
        {
            std::lock_guard<std::mutex> lock(synthMutex);
            synth.setSong(song);
        }
        seekToGame(r);
    }

    void selectPlayingSong() noexcept
    {
        Resident* best = nullptr;
        for (Resident& r : residents)
        {
            if (!r.active || r.unplayable)
                continue;
            if (!best || r.activeSince > best->activeSince
                || (r.activeSince == best->activeSince && r.lastActivityFrame > best->lastActivityFrame))
                best = &r;
        }

        // Engines that don't keep raw pointers: fall back to the newest loaded song.
        if (!best && !pointersEverSeen)
        {
            for (Resident& r : residents)
            {
                if (!r.unplayable && (!best || r.firstSeenFrame > best->firstSeenFrame))
                    best = &r;
            }
        }

        if (!best)
        {
            stopActiveSong();
            return;
        }
        if (activeResidentRamOff != static_cast<s64>(best->ramOff) || !activeSong || activeSong->romIndex != best->romIndex)
            activateResident(*best);
    }
} // anonymous namespace

void onRomLoaded(const melonDS::u8* rom, melonDS::u32 length) noexcept
{
    onShutdown();
    if (!rom || length < 0x200)
        return;
    romPtr = rom;
    romLength = length;

    const u32 fatOffset = rd32(rom + 0x48);
    const u32 fatSize = rd32(rom + 0x4C);
    if (fatOffset < 0x200 || fatSize < 8 || fatOffset + fatSize > length)
        return;

    u32 numSongs = 0;
    for (u32 i = 0; i < fatSize / 8u; ++i)
    {
        const u32 start = rd32(rom + fatOffset + i * 8u);
        const u32 end = rd32(rom + fatOffset + i * 8u + 4u);
        if (start >= length || end > length || end <= start || end - start < 0x80)
            continue;
        const u8* f = rom + start;
        const u32 magic = rd32(f);
        if (magic != kMagicSmdl && magic != kMagicSwdl)
            continue;
        const u32 declared = rd32(f + 8);
        if (rd32(f + 4) != 0 || declared < 0x50 || declared > end - start)
            continue;

        RomDseFile file;
        file.isSmdl = magic == kMagicSmdl;
        file.romOffset = start;
        file.size = declared;
        file.id = rd16(f + 0x0E);
        std::memcpy(file.name, f + 0x20, 16);
        file.name[16] = 0;
        if (file.isSmdl)
        {
            if (!isMagic(f + 0x40, "song"))
                continue;
            ++numSongs;
        }
        else
        {
            file.mainbankId = rd16(f + 0x40);
            const std::vector<ChunkRef> chunks = walkChunks(f, declared, 0x50);
            if (const ChunkRef* pcmd = findChunk(chunks, "pcmd"))
                file.pcmdSize = pcmd->dataEnd - pcmd->dataOffset;
        }
        romFiles.push_back(file);
    }

    dseGame.store(numSongs > 0, std::memory_order_release);
    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Info,
        "FastForwardMusic: indexed %u DSE song(s), %zu DSE file(s) in ROM\n",
        numSongs, romFiles.size());
}

bool isDseGame() noexcept
{
    return dseGame.load(std::memory_order_acquire);
}

namespace
{
    void runDetectionFrame(bool fastForwardActive) noexcept;
}

void onEmulatorFrame(melonDS::NDS& nds, bool fastForwardActive) noexcept
{
    if (!isDseGame())
        return;

    mainRam = nds.MainRAM;
    mainRamMask = nds.MainRAMMask;
    arm7Wram = nds.ARM7WRAM;
    arm7WramSize = nds.ARM7WRAMSize;
    sharedWram = nds.SharedWRAM;
    sharedWramSize = nds.SharedWRAMSize;
    runDetectionFrame(fastForwardActive);
}

namespace
{
void runDetectionFrame(bool fastForwardActive) noexcept
{
    ++frameCounter;
    if (!mainRam || mainRamMask < 0xFFFF)
        return;

    const bool fastForwardStarted = fastForwardActive && !wasFastForward;
    wasFastForward = fastForwardActive;

    const u32 magicInterval = residents.empty() ? 12u : 60u;
    if (frameCounter - lastMagicScanFrame >= magicInterval)
    {
        lastMagicScanFrame = frameCounter;
        scanForLoadedSongs();
    }

    if ((frameCounter & 1u) != 0 && !fastForwardStarted)
        return;

    bool needPointerScan = residentsChanged || frameCounter - lastPointerScanFrame >= 300u;
    if (!needPointerScan && frameCounter - lastPointerScanFrame >= 30u)
    {
        // Songs that were just loaded may not be started yet; look for their track
        // pointers more often for the first ten seconds.
        for (const Resident& r : residents)
        {
            if (r.pointers.empty() && frameCounter - r.firstSeenFrame < 600u)
            {
                needPointerScan = true;
                break;
            }
        }
    }
    if (needPointerScan)
    {
        // Keep the activity state, only refresh where the pointers live.
        lastPointerScanFrame = frameCounter;
        residentsChanged = false;
        scanForTrackPointers();
    }

    updateActivity();
    selectPlayingSong();

    if (fastForwardStarted && activeResidentRamOff >= 0)
    {
        for (const Resident& r : residents)
        {
            if (static_cast<s64>(r.ramOff) == activeResidentRamOff)
            {
                seekToGame(r);
                break;
            }
        }
    }
}
} // anonymous namespace

void disable() noexcept
{
    if (!isDseGame())
        return;
    stopActiveSong();
    residents.clear();
}

void onReset() noexcept
{
    clearDetection();
    wasFastForward = false;
    std::lock_guard<std::mutex> lock(synthMutex);
    synth.clear();
}

void onStateLoaded() noexcept
{
    onReset();
}

void onShutdown() noexcept
{
    onReset();
    romPtr = nullptr;
    romLength = 0;
    romFiles.clear();
    songCache.clear();
    pointersEverSeen = false;
    mainRam = nullptr;
    arm7Wram = nullptr;
    sharedWram = nullptr;
    dseGame.store(false, std::memory_order_release);
}

void onSpuChannelStart(melonDS::u32 channelNum, melonDS::u32 srcAddr, melonDS::u32 loopPos, melonDS::u32 length) noexcept
{
    if (channelNum >= 16 || !isDseGame())
        return;
    ChannelSignature& cs = channelSignatures[channelNum];
    cs = ChannelSignature{};
    if (mainRam && (srcAddr & 0xFF000000u) == 0x02000000u)
    {
        const u32 ramSize = mainRamMask + 1;
        const u32 off = srcAddr & mainRamMask;
        const u32 available = std::min<u32>(ramSize - off, 64u);
        cs.valid = true;
        cs.sig = makeSignature(mainRam + off, available, loopPos + length);
    }
    channelIsBgm[channelNum].store(
        activeForSpuFilter.load(std::memory_order_relaxed) && channelMatchesSong(cs),
        std::memory_order_relaxed);
}

bool hasActiveSongForSpuFilter() noexcept
{
    return activeForSpuFilter.load(std::memory_order_acquire);
}

bool isBgmChannel(melonDS::u32 channelNum) noexcept
{
    return channelNum < 16 && channelIsBgm[channelNum].load(std::memory_order_relaxed);
}

bool renderIfActive(melonDS::s16* audioData, int numFrames, bool mixWithExisting) noexcept
{
    std::lock_guard<std::mutex> lock(synthMutex);
    if (!synth.hasSong())
        return false;
    synth.render(audioData, numFrames, mixWithExisting);
    return true;
}

void advanceSilentFrames(int numFrames) noexcept
{
    std::lock_guard<std::mutex> lock(synthMutex);
    synth.advanceSilent(numFrames);
}

} // namespace MelonDSAndroid::FastForwardMusic::Dse
