#include <xtl.h>
#include <xaudio2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

// stb_vorbis on big-endian Xbox 360:
// Disable the fast float-to-int path which has endian assumptions.
#define STB_VORBIS_NO_FAST_SCALED_FLOAT
#include "stb_vorbis.c"

#include "stb_ds.h"
#include "xaudio2_audio.h"

extern "C" unsigned long __cdecl DbgPrint(const char* format, ...);
extern "C" void Butterscotch_xdkDiagTrace(const char* fmt, ...);

#define XDK_AUDIO_FIX_TAG "clean_xaudio2_gameover_music_cut_v15"
#define XDK_AUDIO_CACHE_LIMIT_BYTES (8u * 1024u * 1024u)
#define XDK_AUDIO_CACHE_SINGLE_LIMIT_BYTES (768u * 1024u)
#define XDK_AUDIO_MAX_CACHED_SOUNDS 4096
#define XDK_AUDIO_BIG_OGG_PCM_BYTES (12u * 1024u * 1024u)
#define XDK_AUDIO_DECODE_CHUNK_FRAMES 4096
#define XDK_AUDIO_STREAM_BUFFER_COUNT 4
#define XDK_AUDIO_STREAM_BUFFER_FRAMES 4096

static void audioTrace(bool fileLog, const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    DbgPrint("%s\n", line);
    if (fileLog) {
        Butterscotch_xdkDiagTrace("%s", line);
    }
}

static uint16_t readLe16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t readLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t nowMs(void) {
    return GetTickCount();
}

static bool startsWithIgnoreCase(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static bool isMusicSoundName(const char* name) {
    return startsWithIgnoreCase(name, "mus_");
}

static bool isBattleMusicSoundName(const char* name) {
    return startsWithIgnoreCase(name, "mus_battle") ||
        startsWithIgnoreCase(name, "mus_prebattle") ||
        startsWithIgnoreCase(name, "mus_ghostbattle");
}

static bool isHouseMusicSoundName(const char* name) {
    return startsWithIgnoreCase(name, "mus_house");
}

static bool canLayerMusicSoundNames(const char* activeName, const char* newName) {
    return isHouseMusicSoundName(activeName) && isHouseMusicSoundName(newName);
}

static bool isBattleRoomName(const char* name) {
    return startsWithIgnoreCase(name, "room_battle");
}

static bool isGameOverRoomName(const char* name) {
    return startsWithIgnoreCase(name, "room_gameover");
}

static short* resamplePcm16(short* input, uint32_t inputFrames, uint16_t channels, uint32_t inputRate, uint32_t outputRate, uint32_t* outFrames) {
    if (outFrames) *outFrames = inputFrames;
    if (!input || inputFrames == 0 || channels == 0 || inputRate == 0 || outputRate == 0 || inputRate == outputRate) {
        return input;
    }

    uint32_t outputFrames = (uint32_t)(((uint64_t)inputFrames * outputRate + inputRate - 1) / inputRate);
    short* output = (short*)malloc(outputFrames * channels * sizeof(short));
    if (!output) {
        audioTrace(true, "AUD2: resample malloc failed inFrames=%u ch=%u inRate=%u outRate=%u",
            inputFrames, channels, inputRate, outputRate);
        return input;
    }

    for (uint32_t i = 0; i < outputFrames; i++) {
        uint64_t srcFixed = (((uint64_t)i * inputRate) << 16) / outputRate;
        uint32_t srcIndex = (uint32_t)(srcFixed >> 16);
        uint32_t frac = (uint32_t)(srcFixed & 0xFFFF);
        if (srcIndex >= inputFrames - 1) {
            srcIndex = inputFrames - 1;
            frac = 0;
        }

        for (uint16_t ch = 0; ch < channels; ch++) {
            int32_t a = input[srcIndex * channels + ch];
            int32_t b = input[((srcIndex + 1 < inputFrames) ? (srcIndex + 1) : srcIndex) * channels + ch];
            output[i * channels + ch] = (short)(a + (((b - a) * (int32_t)frac) >> 16));
        }
    }

    free(input);
    if (outFrames) *outFrames = outputFrames;
    return output;
}

struct XdkDecodedSound {
    bool valid;
    bool failed;
    uint8_t* pcmData;
    uint32_t pcmSize;
    uint32_t sampleRate;
    uint16_t channels;
    uint32_t sampleFrames;
};

struct XdkSoundInstance {
    bool active;
    bool paused;
    bool loop;
    int32_t soundIndex;
    int32_t instanceId;
    int32_t priority;

    IXAudio2SourceVoice* pVoice;
    XdkDecodedSound* decoded;
    bool ownsDecoded;
    bool music;
    bool streaming;

    stb_vorbis* streamVorbis;
    uint32_t streamSampleRate;
    uint16_t streamChannels;
    uint32_t streamTotalFrames;
    short* streamBuffers[XDK_AUDIO_STREAM_BUFFER_COUNT];
    uint32_t streamSubmitted;
    bool streamEof;

    uint32_t startedTick;
    uint32_t pauseStartedTick;
    uint32_t pausedTotalMs;

    float currentGain;
    float targetGain;
    float startGain;
    float fadeTotalTime;
    float fadeTimeRemaining;
    float pitch;
    float soundVolume;
    float soundPitch;
};

struct XdkInstanceArray {
    XdkSoundInstance instances[XDK_MAX_SOUND_INSTANCES];
};

static XdkDecodedSound gSoundCache[XDK_AUDIO_MAX_CACHED_SOUNDS];
static uint32_t gSoundCacheBytes = 0;
static int32_t gSuspendedRoomMusicIndex = -1;
static bool gSuspendedRoomMusicLoop = true;
static bool gResumingSuspendedMusic = false;
static int32_t gAwaitingBattleExitMusicIndex = -1;
static bool gAwaitingBattleExitMusicLoop = true;
static bool gInBattleRoom = false;

static inline XdkInstanceArray* Instances(XdkAudioSystem* xa) {
    return (XdkInstanceArray*)xa->instanceData;
}

static bool isInstanceId(int32_t value) {
    return value >= XDK_SOUND_INSTANCE_ID_BASE;
}

static uint32_t instanceElapsedMs(const XdkSoundInstance* inst) {
    uint32_t elapsed = nowMs() - inst->startedTick;
    if (inst->paused) {
        elapsed -= nowMs() - inst->pauseStartedTick;
    }
    if (elapsed > inst->pausedTotalMs) {
        elapsed -= inst->pausedTotalMs;
    } else {
        elapsed = 0;
    }
    return elapsed;
}

static uint32_t decodedDurationMs(const XdkDecodedSound* decoded) {
    if (!decoded || decoded->sampleRate == 0) return 0;
    return (uint32_t)(((uint64_t)decoded->sampleFrames * 1000u) / decoded->sampleRate);
}

static uint32_t instanceDurationMs(const XdkSoundInstance* inst) {
    if (!inst) return 0;
    if (inst->streaming) {
        if (inst->streamSampleRate == 0) return 0;
        return (uint32_t)(((uint64_t)inst->streamTotalFrames * 1000u) / inst->streamSampleRate);
    }
    return decodedDurationMs(inst->decoded);
}

static float instanceDurationSeconds(const XdkSoundInstance* inst) {
    if (!inst) return 0.0f;
    if (inst->streaming) {
        return inst->streamSampleRate ? ((float)inst->streamTotalFrames / (float)inst->streamSampleRate) : 0.0f;
    }
    if (!inst->decoded || inst->decoded->sampleRate == 0) return 0.0f;
    return (float)inst->decoded->sampleFrames / (float)inst->decoded->sampleRate;
}

static float decodedDurationSeconds(const XdkDecodedSound* decoded) {
    if (!decoded || decoded->sampleRate == 0) return 0.0f;
    return (float)decoded->sampleFrames / (float)decoded->sampleRate;
}

static XdkSoundInstance* findById(XdkAudioSystem* xa, int32_t id) {
    int32_t idx = id - XDK_SOUND_INSTANCE_ID_BASE;
    if (idx < 0 || idx >= XDK_MAX_SOUND_INSTANCES) return NULL;
    XdkSoundInstance* inst = &Instances(xa)->instances[idx];
    if (!inst->active || inst->instanceId != id) return NULL;
    return inst;
}

static bool instanceLooksPlaying(XdkSoundInstance* inst) {
    if (!inst || !inst->active || inst->paused || !inst->pVoice) return false;
    if (inst->loop) return true;

    uint32_t duration = instanceDurationMs(inst);
    if (duration > 0 && instanceElapsedMs(inst) < duration) {
        return true;
    }

    XAUDIO2_VOICE_STATE state;
    inst->pVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued > 0;
}

static void destroyInstance(XdkSoundInstance* inst) {
    if (!inst) return;
    if (inst->pVoice) {
        inst->pVoice->Stop(0);
        inst->pVoice->FlushSourceBuffers();
        inst->pVoice->DestroyVoice();
        inst->pVoice = NULL;
    }
    if (inst->ownsDecoded && inst->decoded) {
        free(inst->decoded->pcmData);
        free(inst->decoded);
        inst->decoded = NULL;
        inst->ownsDecoded = false;
    }
    if (inst->streamVorbis) {
        stb_vorbis_close(inst->streamVorbis);
        inst->streamVorbis = NULL;
    }
    for (int i = 0; i < XDK_AUDIO_STREAM_BUFFER_COUNT; i++) {
        if (inst->streamBuffers[i]) {
            free(inst->streamBuffers[i]);
            inst->streamBuffers[i] = NULL;
        }
    }
    memset(inst, 0, sizeof(XdkSoundInstance));
}

static XdkSoundInstance* findFreeSlot(XdkAudioSystem* xa) {
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (!arr->instances[i].active) return &arr->instances[i];
    }

    XdkSoundInstance* best = NULL;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active || inst->loop) continue;
        if (!instanceLooksPlaying(inst)) {
            if (best == NULL || inst->priority < best->priority) {
                best = inst;
            }
        }
    }
    if (best != NULL) {
        destroyInstance(best);
        return best;
    }

    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->loop && (best == NULL || inst->priority < best->priority)) {
            best = inst;
        }
    }
    if (best != NULL) {
        destroyInstance(best);
        return best;
    }
    return NULL;
}

static DataWin* getAudioGroup(XdkAudioSystem* xa, int32_t groupIndex) {
    if (!xa->base.audioGroups || arrlen(xa->base.audioGroups) == 0) return NULL;
    if (groupIndex >= 0 && groupIndex < (int32_t)arrlen(xa->base.audioGroups) && xa->base.audioGroups[groupIndex]) {
        return xa->base.audioGroups[groupIndex];
    }
    return xa->base.audioGroups[0];
}

static Sound* soundAtSafe(DataWin* dw, int32_t idx) {
    static Sound dummy;
    static bool dummyInit = false;
    if (!dummyInit) {
        memset(&dummy, 0, sizeof(dummy));
        dummy.name = (char*)"";
        dummy.volume = 1.0f;
        dummy.pitch = 1.0f;
        dummyInit = true;
    }
    if (!dw || idx < 0 || (uint32_t)idx >= dw->sond.count) return &dummy;
    return dw->sond.sounds + idx;
}

#define XDK_STREAM_HANDLE_BASE 50000
#define XDK_MAX_STREAMS 32
static char gStreamPaths[XDK_MAX_STREAMS][260];
static bool gStreamUsed[XDK_MAX_STREAMS];

static char* resolveExternalPath(XdkAudioSystem* xa, Sound* sound) {
    const char* file = sound->file;
    if (!file || !file[0] || !xa->fileSystem) return NULL;

    char filename[512];
    if (strchr(file, '.') != NULL) {
        _snprintf(filename, sizeof(filename), "%s", file);
    } else {
        _snprintf(filename, sizeof(filename), "%s.ogg", file);
    }
    filename[sizeof(filename) - 1] = '\0';
    return xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
}

static bool soundUsesAudo(Sound* sound) {
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    return !isRegular || isEmbedded || isCompressed;
}

static bool shouldStreamExternalMusic(Sound* sound) {
    if (!sound || !isMusicSoundName(sound->name) || soundUsesAudo(sound)) return false;
    return true;
}

static bool readWholeFile(const char* path, uint8_t** outData, int* outSize) {
    *outData = NULL;
    *outSize = 0;
    FILE* f = fopen(path, "rb");
    Butterscotch_xdkDiagTrace("AUD2: try file '%s' -> %s", path, f ? "OK" : "MISSING");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return false;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        return false;
    }
    *outData = data;
    *outSize = (int)size;
    return true;
}

static bool decodeWav16(const uint8_t* data, int size, XdkDecodedSound* decoded, const char* name) {
    if (size < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    const uint8_t* fmtData = NULL;
    uint32_t fmtSize = 0;
    const uint8_t* pcmData = NULL;
    uint32_t pcmSize = 0;

    int offset = 12;
    while (offset + 8 <= size) {
        const uint8_t* chunk = data + offset;
        uint32_t chunkSize = readLe32(chunk + 4);
        int payload = offset + 8;
        if (payload + (int)chunkSize > size) break;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            fmtData = data + payload;
            fmtSize = chunkSize;
        } else if (memcmp(chunk, "data", 4) == 0) {
            pcmData = data + payload;
            pcmSize = chunkSize;
        }

        offset = payload + (int)chunkSize + (chunkSize & 1u);
    }

    if (!fmtData || !pcmData || fmtSize < 16) {
        audioTrace(true, "AUD2: WAV parse failed missing fmt/data name=%s", name ? name : "?");
        return false;
    }

    uint16_t format = readLe16(fmtData);
    uint16_t channels = readLe16(fmtData + 2);
    uint32_t rate = readLe32(fmtData + 4);
    uint16_t bits = readLe16(fmtData + 14);
    if (format != WAVE_FORMAT_PCM || channels == 0 || channels > 2 || rate == 0 || bits != 16 || pcmSize == 0) {
        audioTrace(true, "AUD2: WAV unsupported name=%s fmt=%u ch=%u rate=%u bits=%u bytes=%u",
            name ? name : "?", format, channels, rate, bits, pcmSize);
        return false;
    }

    uint8_t* out = (uint8_t*)malloc(pcmSize);
    if (!out) return false;
    memcpy(out, pcmData, pcmSize);

    // The existing 360 port used big-endian 16-bit PCM for XAudio2.
    // WAV files store little-endian samples, so swap them on PowerPC.
    uint16_t* samples = (uint16_t*)out;
    uint32_t sampleWords = pcmSize / 2;
    for (uint32_t i = 0; i < sampleWords; i++) {
        uint16_t v = samples[i];
        samples[i] = (uint16_t)((v >> 8) | (v << 8));
    }

    decoded->pcmData = out;
    decoded->pcmSize = pcmSize;
    decoded->sampleRate = rate;
    decoded->channels = channels;
    decoded->sampleFrames = pcmSize / (channels * 2);
    decoded->valid = true;
    decoded->failed = false;
    return true;
}

static bool decodeOgg(const uint8_t* data, int size, XdkDecodedSound* decoded, const char* name) {
    int stbErr = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory((unsigned char*)data, size, &stbErr, NULL);
    if (!vorbis) {
        audioTrace(true, "AUD2: vorbis open failed name=%s err=%d size=%d", name ? name : "?", stbErr, size);
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    int totalFrames = stb_vorbis_stream_length_in_samples(vorbis);
    if (info.channels <= 0 || info.channels > 2 || info.sample_rate <= 0 || totalFrames <= 0) {
        stb_vorbis_close(vorbis);
        return false;
    }

    uint32_t bytes = (uint32_t)(totalFrames * info.channels * sizeof(short));
    if (bytes > XDK_AUDIO_BIG_OGG_PCM_BYTES) {
        uint32_t decimation = 0;
        uint32_t outputFrames = 0;
        uint32_t outputBytes = 0;
        short* pcm = NULL;
        for (uint32_t tryDecimation = 2; tryDecimation <= 8; tryDecimation *= 2) {
            decimation = tryDecimation;
            outputFrames = (uint32_t)((totalFrames + (int)decimation - 1) / (int)decimation);
            outputBytes = outputFrames * (uint32_t)info.channels * sizeof(short);
            pcm = (short*)malloc(outputBytes);
            if (pcm) {
                if (decimation > 2) {
                    audioTrace(true, "AUD2: using 1/%u-rate ogg fallback name=%s outBytes=%u fullBytes=%u",
                        decimation, name ? name : "?", outputBytes, bytes);
                }
                break;
            }
        }
        short* chunk = (short*)malloc(XDK_AUDIO_DECODE_CHUNK_FRAMES * info.channels * sizeof(short));
        if (!pcm || !chunk) {
            audioTrace(true, "AUD2: vorbis downsample malloc failed name=%s div=%u outBytes=%u chunkBytes=%u fullBytes=%u",
                name ? name : "?", decimation, outputBytes,
                (unsigned)(XDK_AUDIO_DECODE_CHUNK_FRAMES * info.channels * sizeof(short)), bytes);
            if (pcm) free(pcm);
            if (chunk) free(chunk);
            stb_vorbis_close(vorbis);
            return false;
        }

        uint32_t srcFrame = 0;
        uint32_t dstFrame = 0;
        for (;;) {
            int frames = stb_vorbis_get_samples_short_interleaved(vorbis, info.channels,
                chunk, XDK_AUDIO_DECODE_CHUNK_FRAMES * info.channels);
            if (frames <= 0) break;

            for (int frame = 0; frame < frames && dstFrame < outputFrames; frame++, srcFrame++) {
                if ((srcFrame % decimation) != 0) continue;
                for (int ch = 0; ch < info.channels; ch++) {
                    pcm[dstFrame * info.channels + ch] = chunk[frame * info.channels + ch];
                }
                dstFrame++;
            }
        }

        free(chunk);
        stb_vorbis_close(vorbis);
        if (dstFrame == 0) {
            free(pcm);
            return false;
        }

        decoded->pcmData = (uint8_t*)pcm;
        decoded->pcmSize = dstFrame * (uint32_t)info.channels * sizeof(short);
        decoded->sampleRate = (uint32_t)(info.sample_rate / (int)decimation);
        decoded->channels = (uint16_t)info.channels;
        decoded->sampleFrames = dstFrame;
        decoded->valid = true;
        decoded->failed = false;
        audioTrace(true, "AUD2: decoded 1/%u-rate ogg name=%s fullBytes=%u rate=%d->%u frames=%d->%u bytes=%u",
            decimation, name ? name : "?", bytes,
            info.sample_rate, decoded->sampleRate, totalFrames, decoded->sampleFrames, decoded->pcmSize);
        return true;
    }

    short* pcm = (short*)malloc(bytes);
    if (!pcm) {
        audioTrace(true, "AUD2: vorbis pcm malloc failed name=%s bytes=%u frames=%d ch=%d",
            name ? name : "?", bytes, totalFrames, info.channels);
        stb_vorbis_close(vorbis);
        return false;
    }

    int frames = stb_vorbis_get_samples_short_interleaved(vorbis, info.channels, pcm, totalFrames * info.channels);
    stb_vorbis_close(vorbis);
    if (frames <= 0) {
        free(pcm);
        return false;
    }

    decoded->pcmData = (uint8_t*)pcm;
    decoded->pcmSize = (uint32_t)(frames * info.channels * sizeof(short));
    decoded->sampleRate = (uint32_t)info.sample_rate;
    decoded->channels = (uint16_t)info.channels;
    decoded->sampleFrames = (uint32_t)frames;
    decoded->valid = true;
    decoded->failed = false;
    return true;
}

static bool decodeAudioBytes(const uint8_t* data, int size, XdkDecodedSound* decoded, const char* name) {
    memset(decoded, 0, sizeof(XdkDecodedSound));
    if (!data || size <= 0) return false;
    bool ok;
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0) {
        ok = decodeWav16(data, size, decoded, name);
    } else {
        ok = decodeOgg(data, size, decoded, name);
    }
    if (!ok) return false;

    return true;
}

static XdkDecodedSound* decodeSoundCached(XdkAudioSystem* xa, int32_t soundIndex, bool logToFile, bool* outOwned) {
    if (outOwned) *outOwned = false;
    DataWin* baseDw = getAudioGroup(xa, 0);
    if (!baseDw || soundIndex < 0 || (uint32_t)soundIndex >= baseDw->sond.count) return NULL;

    XdkDecodedSound* cache = NULL;
    if (soundIndex >= 0 && soundIndex < XDK_AUDIO_MAX_CACHED_SOUNDS) {
        cache = &gSoundCache[soundIndex];
        if (cache->valid) return cache;
        if (cache->failed) return NULL;
    }

    Sound* baseSound = soundAtSafe(baseDw, soundIndex);
    DataWin* dw = getAudioGroup(xa, baseSound->audioGroup);
    Sound* sound = baseSound;
    if (dw && dw != baseDw && (uint32_t)soundIndex < dw->sond.count) {
        sound = soundAtSafe(dw, soundIndex);
    }
    if (!dw) dw = baseDw;

    uint8_t* bytes = NULL;
    int byteCount = 0;
    bool freeBytes = false;
    bool inAudo = soundUsesAudo(sound);

    audioTrace(logToFile, "AUD2: decode idx=%d name=%s flags=0x%X group=%d audioFile=%d inAudo=%d file=%s",
        soundIndex, sound->name ? sound->name : "?", (unsigned)sound->flags,
        sound->audioGroup, sound->audioFile, inAudo ? 1 : 0, sound->file ? sound->file : "(null)");

    if (inAudo) {
        if (sound->audioFile < 0 || !dw || (uint32_t)sound->audioFile >= dw->audo.count) {
            audioTrace(true, "AUD2: invalid AUDO idx=%d name=%s audioFile=%d count=%u",
                soundIndex, sound->name ? sound->name : "?", sound->audioFile, dw ? dw->audo.count : 0);
            if (cache) cache->failed = true;
            return NULL;
        }
        AudioEntry* entry = &dw->audo.entries[sound->audioFile];
        bytes = entry->data;
        byteCount = (int)entry->dataSize;
    } else {
        char* path = resolveExternalPath(xa, sound);
        if (!path) {
            audioTrace(true, "AUD2: no external path idx=%d name=%s", soundIndex, sound->name ? sound->name : "?");
            if (cache) cache->failed = true;
            return NULL;
        }
        audioTrace(logToFile, "AUD2: external path idx=%d %s", soundIndex, path);
        if (!readWholeFile(path, &bytes, &byteCount)) {
            audioTrace(true, "AUD2: external read failed idx=%d path=%s", soundIndex, path);
            free(path);
            if (cache) cache->failed = true;
            return NULL;
        }
        free(path);
        freeBytes = true;
    }

    XdkDecodedSound decoded;
    bool ok = decodeAudioBytes(bytes, byteCount, &decoded, sound->name);
    if (freeBytes) free(bytes);
    if (!ok) return NULL;

    audioTrace(logToFile, "AUD2: decoded idx=%d name=%s ch=%u rate=%u frames=%u bytes=%u seconds=%.3f",
        soundIndex, sound->name ? sound->name : "?", decoded.channels, decoded.sampleRate,
        decoded.sampleFrames, decoded.pcmSize, decodedDurationSeconds(&decoded));

    bool isMusic = isMusicSoundName(sound->name);
    bool allowCache = decoded.pcmSize <= XDK_AUDIO_CACHE_SINGLE_LIMIT_BYTES;
    if (isMusic && decoded.pcmSize > (512u * 1024u)) {
        allowCache = false;
    }

    if (cache &&
        allowCache &&
        decoded.pcmSize <= XDK_AUDIO_CACHE_SINGLE_LIMIT_BYTES &&
        gSoundCacheBytes + decoded.pcmSize <= XDK_AUDIO_CACHE_LIMIT_BYTES) {
        *cache = decoded;
        gSoundCacheBytes += decoded.pcmSize;
        return cache;
    }

    XdkDecodedSound* owned = (XdkDecodedSound*)malloc(sizeof(XdkDecodedSound));
    if (!owned) {
        audioTrace(true, "AUD2: decoded struct malloc failed idx=%d bytes=%u", soundIndex, decoded.pcmSize);
        free(decoded.pcmData);
        if (cache) cache->failed = true;
        return NULL;
    }
    *owned = decoded;
    if (outOwned) *outOwned = true;
    audioTrace(logToFile, "AUD2: uncached decoded idx=%d bytes=%u cacheUsed=%u limit=%u",
        soundIndex, decoded.pcmSize, gSoundCacheBytes, (unsigned)XDK_AUDIO_CACHE_LIMIT_BYTES);
    return owned;
}

static HRESULT submitFromFrame(XdkSoundInstance* inst, uint32_t frameOffset) {
    if (!inst || !inst->pVoice || !inst->decoded || !inst->decoded->valid) return E_FAIL;

    XdkDecodedSound* decoded = inst->decoded;
    uint32_t blockAlign = decoded->channels * 2;
    if (blockAlign == 0) return E_FAIL;
    if (frameOffset >= decoded->sampleFrames) frameOffset = decoded->sampleFrames - 1;

    uint32_t byteOffset = frameOffset * blockAlign;
    XAUDIO2_BUFFER buf;
    memset(&buf, 0, sizeof(buf));
    buf.AudioBytes = decoded->pcmSize - byteOffset;
    buf.pAudioData = decoded->pcmData + byteOffset;
    if (inst->loop) {
        buf.LoopCount = XAUDIO2_LOOP_INFINITE;
        buf.Flags = 0;
    } else {
        buf.Flags = XAUDIO2_END_OF_STREAM;
    }
    return inst->pVoice->SubmitSourceBuffer(&buf);
}

static uint32_t streamQueuedBuffers(XdkSoundInstance* inst) {
    if (!inst || !inst->pVoice) return 0;
    XAUDIO2_VOICE_STATE state;
    inst->pVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued;
}

static bool submitStreamingBuffers(XdkSoundInstance* inst, bool logToFile) {
    if (!inst || !inst->streaming || !inst->pVoice || !inst->streamVorbis) return false;
    if (inst->streamEof) return true;

    uint32_t queued = streamQueuedBuffers(inst);
    while (queued < XDK_AUDIO_STREAM_BUFFER_COUNT) {
        uint32_t bufferIndex = inst->streamSubmitted % XDK_AUDIO_STREAM_BUFFER_COUNT;
        short* buffer = inst->streamBuffers[bufferIndex];
        if (!buffer) return false;

        int frames = stb_vorbis_get_samples_short_interleaved(inst->streamVorbis,
            inst->streamChannels, buffer, XDK_AUDIO_STREAM_BUFFER_FRAMES * inst->streamChannels);

        if (frames <= 0) {
            if (inst->loop) {
                if (!stb_vorbis_seek_start(inst->streamVorbis)) {
                    audioTrace(true, "AUD2: stream seek_start failed idx=%d", inst->soundIndex);
                    inst->streamEof = true;
                    return false;
                }
                frames = stb_vorbis_get_samples_short_interleaved(inst->streamVorbis,
                    inst->streamChannels, buffer, XDK_AUDIO_STREAM_BUFFER_FRAMES * inst->streamChannels);
                if (frames <= 0) {
                    audioTrace(true, "AUD2: stream loop refill failed idx=%d", inst->soundIndex);
                    inst->streamEof = true;
                    return false;
                }
            } else {
                inst->streamEof = true;
                break;
            }
        }

        XAUDIO2_BUFFER xaBuf;
        memset(&xaBuf, 0, sizeof(xaBuf));
        xaBuf.AudioBytes = frames * inst->streamChannels * sizeof(short);
        xaBuf.pAudioData = (BYTE*)buffer;
        HRESULT hr = inst->pVoice->SubmitSourceBuffer(&xaBuf);
        if (FAILED(hr)) {
            audioTrace(true, "AUD2: stream SubmitSourceBuffer failed idx=%d hr=0x%08X bytes=%u",
                inst->soundIndex, (unsigned)hr, xaBuf.AudioBytes);
            inst->streamEof = true;
            return false;
        }

        inst->streamSubmitted++;
        queued++;
    }

    audioTrace(logToFile, "AUD2: stream filled idx=%d queued=%u submitted=%u eof=%d",
        inst->soundIndex, queued, inst->streamSubmitted, inst->streamEof ? 1 : 0);
    return true;
}

static bool createVoiceForInstance(XdkAudioSystem* xa, XdkSoundInstance* inst, bool logToFile) {
    XdkDecodedSound* decoded = inst->decoded;
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = decoded->channels;
    wfx.nSamplesPerSec = decoded->sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = decoded->channels * 2;
    wfx.nAvgBytesPerSec = decoded->sampleRate * wfx.nBlockAlign;
    wfx.cbSize = 0;

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    HRESULT hr = pXA->CreateSourceVoice(&inst->pVoice, &wfx);
    if (FAILED(hr) || !inst->pVoice) {
        audioTrace(true, "AUD2: CreateSourceVoice failed idx=%d hr=0x%08X ch=%u rate=%u",
            inst->soundIndex, (unsigned)hr, decoded->channels, decoded->sampleRate);
        return false;
    }

    hr = submitFromFrame(inst, 0);
    if (FAILED(hr)) {
        audioTrace(true, "AUD2: SubmitSourceBuffer failed idx=%d hr=0x%08X", inst->soundIndex, (unsigned)hr);
        return false;
    }

    float ratio = inst->pitch * inst->soundPitch;
    if (ratio <= 0.0f) ratio = 1.0f;
    HRESULT ratioHr = inst->pVoice->SetFrequencyRatio(ratio);
    float actualRatio = -1.0f;
    inst->pVoice->GetFrequencyRatio(&actualRatio);
    float finalVolume = inst->currentGain * inst->soundVolume * xa->masterGain;
    bool startupFade = inst->music && decodedDurationMs(decoded) > 1000;
    inst->pVoice->SetVolume(startupFade ? 0.0f : finalVolume);

    hr = inst->pVoice->Start(0);
    if (startupFade) {
        inst->startGain = 0.0f;
        inst->targetGain = inst->currentGain;
        inst->currentGain = 0.0f;
        inst->fadeTotalTime = 0.08f;
        inst->fadeTimeRemaining = 0.08f;
    }
    audioTrace(logToFile, "AUD2: start idx=%d instance=%d loop=%d voiceRate=%u ratio=%.5f ratioHr=0x%08X actual=%.5f fade=%d startHr=0x%08X",
        inst->soundIndex, inst->instanceId, inst->loop ? 1 : 0, decoded->sampleRate,
        ratio, (unsigned)ratioHr, actualRatio, startupFade ? 1 : 0, (unsigned)hr);
    return SUCCEEDED(hr);
}

static bool createStreamingVoiceForInstance(XdkAudioSystem* xa, XdkSoundInstance* inst, const char* path, bool logToFile) {
    int err = 0;
    inst->streamVorbis = stb_vorbis_open_filename(path, &err, NULL);
    if (!inst->streamVorbis) {
        audioTrace(true, "AUD2: stream open failed idx=%d path=%s err=%d",
            inst->soundIndex, path ? path : "?", err);
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(inst->streamVorbis);
    int totalFrames = stb_vorbis_stream_length_in_samples(inst->streamVorbis);
    if (info.channels <= 0 || info.channels > 2 || info.sample_rate <= 0) {
        audioTrace(true, "AUD2: stream unsupported idx=%d ch=%d rate=%d",
            inst->soundIndex, info.channels, info.sample_rate);
        return false;
    }

    inst->streaming = true;
    inst->streamChannels = (uint16_t)info.channels;
    inst->streamSampleRate = (uint32_t)info.sample_rate;
    inst->streamTotalFrames = totalFrames > 0 ? (uint32_t)totalFrames : 0;
    inst->streamSubmitted = 0;
    inst->streamEof = false;

    for (int i = 0; i < XDK_AUDIO_STREAM_BUFFER_COUNT; i++) {
        inst->streamBuffers[i] = (short*)malloc(XDK_AUDIO_STREAM_BUFFER_FRAMES * inst->streamChannels * sizeof(short));
        if (!inst->streamBuffers[i]) {
            audioTrace(true, "AUD2: stream buffer malloc failed idx=%d buffer=%d bytes=%u",
                inst->soundIndex, i,
                (unsigned)(XDK_AUDIO_STREAM_BUFFER_FRAMES * inst->streamChannels * sizeof(short)));
            return false;
        }
    }

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = inst->streamChannels;
    wfx.nSamplesPerSec = inst->streamSampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = inst->streamChannels * 2;
    wfx.nAvgBytesPerSec = inst->streamSampleRate * wfx.nBlockAlign;

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    HRESULT hr = pXA->CreateSourceVoice(&inst->pVoice, &wfx);
    if (FAILED(hr) || !inst->pVoice) {
        audioTrace(true, "AUD2: stream CreateSourceVoice failed idx=%d hr=0x%08X ch=%u rate=%u",
            inst->soundIndex, (unsigned)hr, inst->streamChannels, inst->streamSampleRate);
        return false;
    }

    if (!submitStreamingBuffers(inst, false)) {
        return false;
    }

    float ratio = inst->pitch * inst->soundPitch;
    if (ratio <= 0.0f) ratio = 1.0f;
    HRESULT ratioHr = inst->pVoice->SetFrequencyRatio(ratio);
    float actualRatio = -1.0f;
    inst->pVoice->GetFrequencyRatio(&actualRatio);
    float finalVolume = inst->currentGain * inst->soundVolume * xa->masterGain;
    inst->pVoice->SetVolume(0.0f);

    hr = inst->pVoice->Start(0);
    inst->startGain = 0.0f;
    inst->targetGain = inst->currentGain;
    inst->currentGain = 0.0f;
    inst->fadeTotalTime = 0.08f;
    inst->fadeTimeRemaining = 0.08f;

    audioTrace(logToFile, "AUD2: stream start idx=%d instance=%d loop=%d rate=%u ch=%u totalFrames=%u queued=%u ratio=%.5f ratioHr=0x%08X actual=%.5f startHr=0x%08X vol=%.3f",
        inst->soundIndex, inst->instanceId, inst->loop ? 1 : 0, inst->streamSampleRate,
        inst->streamChannels, inst->streamTotalFrames, streamQueuedBuffers(inst),
        ratio, (unsigned)ratioHr, actualRatio, (unsigned)hr, finalVolume);
    return SUCCEEDED(hr);
}

static int32_t xdkPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop);

static void rememberSuspendedRoomMusic(XdkSoundInstance* active, Sound* activeSound, Sound* newSound) {
    if (gResumingSuspendedMusic || !active || !activeSound || !newSound) return;
    if (!isBattleMusicSoundName(newSound->name)) return;
    if (!isMusicSoundName(activeSound->name) || isBattleMusicSoundName(activeSound->name)) return;

    gSuspendedRoomMusicIndex = active->soundIndex;
    gSuspendedRoomMusicLoop = active->loop;
    audioTrace(true, "AUD2: suspend room music idx=%d name=%s loop=%d for=%s",
        gSuspendedRoomMusicIndex, activeSound->name ? activeSound->name : "?",
        gSuspendedRoomMusicLoop ? 1 : 0, newSound->name ? newSound->name : "?");
}

static void maybeResumeSuspendedRoomMusic(AudioSystem* audio, const char* stoppedName) {
    if (gResumingSuspendedMusic || gSuspendedRoomMusicIndex < 0) return;
    if (!isBattleMusicSoundName(stoppedName)) return;

    if (gInBattleRoom) {
        gAwaitingBattleExitMusicIndex = gSuspendedRoomMusicIndex;
        gAwaitingBattleExitMusicLoop = gSuspendedRoomMusicLoop;
        audioTrace(true, "AUD2: defer suspended room music idx=%d loop=%d until battle room exit after=%s",
            gAwaitingBattleExitMusicIndex, gAwaitingBattleExitMusicLoop ? 1 : 0,
            stoppedName ? stoppedName : "?");
        gSuspendedRoomMusicIndex = -1;
        gSuspendedRoomMusicLoop = true;
        return;
    }

    gAwaitingBattleExitMusicIndex = gSuspendedRoomMusicIndex;
    gAwaitingBattleExitMusicLoop = gSuspendedRoomMusicLoop;
    gSuspendedRoomMusicIndex = -1;
    gSuspendedRoomMusicLoop = true;

    audioTrace(true, "AUD2: battle music stopped outside battle room; resume room music now after=%s",
        stoppedName ? stoppedName : "?");
    XdkAudioSystem_onRoomChanged(audio, -1, NULL);
}

void XdkAudioSystem_onRoomChanged(AudioSystem* audio, int32_t roomIndex, const char* roomName) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    bool wasInBattleRoom = gInBattleRoom;
    gInBattleRoom = isBattleRoomName(roomName);

    if (roomName) {
        audioTrace(true, "AUD2: room changed idx=%d name=%s battle=%d awaiting=%d",
            roomIndex, roomName, gInBattleRoom ? 1 : 0, gAwaitingBattleExitMusicIndex);
    }

    if (isGameOverRoomName(roomName)) {
        XdkInstanceArray* arr = Instances(xa);
        DataWin* dw = getAudioGroup(xa, 0);
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            XdkSoundInstance* inst = &arr->instances[i];
            if (!inst->active || !inst->music) continue;
            const char* stoppedName = NULL;
            if (dw && inst->soundIndex >= 0 && (uint32_t)inst->soundIndex < dw->sond.count) {
                stoppedName = soundAtSafe(dw, inst->soundIndex)->name;
            }
            audioTrace(true, "AUD2: gameover cut music idx=%d name=%s instance=%d",
                inst->soundIndex, stoppedName ? stoppedName : "?", inst->instanceId);
            destroyInstance(inst);
        }
        gSuspendedRoomMusicIndex = -1;
        gSuspendedRoomMusicLoop = true;
        gAwaitingBattleExitMusicIndex = -1;
        gAwaitingBattleExitMusicLoop = true;
    }

    if (gResumingSuspendedMusic || gAwaitingBattleExitMusicIndex < 0 || gInBattleRoom) return;
    if (!wasInBattleRoom && roomName != NULL) return;

    int32_t resumeIndex = gAwaitingBattleExitMusicIndex;
    bool resumeLoop = gAwaitingBattleExitMusicLoop;
    gAwaitingBattleExitMusicIndex = -1;
    gAwaitingBattleExitMusicLoop = true;

    gResumingSuspendedMusic = true;
    audioTrace(true, "AUD2: resume suspended room music idx=%d loop=%d on room=%s",
        resumeIndex, resumeLoop ? 1 : 0, roomName ? roomName : "(already outside battle)");
    int32_t instance = xdkPlaySound(audio, resumeIndex, 10, resumeLoop);
    audioTrace(true, "AUD2: resume result idx=%d instance=%d", resumeIndex, instance);
    gResumingSuspendedMusic = false;
}

static void xdkAudioInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    arrput(xa->base.audioGroups, dataWin);
    xa->fileSystem = fileSystem;
    xa->masterGain = 1.0f;

    HRESULT hr = XAudio2Create((IXAudio2**)&xa->pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        audioTrace(true, "AUD2: XAudio2Create failed hr=0x%08X", (unsigned)hr);
        return;
    }

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    hr = pXA->CreateMasteringVoice((IXAudio2MasteringVoice**)&xa->pMasterVoice,
        XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL);
    if (FAILED(hr)) {
        audioTrace(true, "AUD2: CreateMasteringVoice failed hr=0x%08X", (unsigned)hr);
        return;
    }

    xa->initialized = true;
    audioTrace(true, "AUD2: clean audio backend %s", XDK_AUDIO_FIX_TAG);
}

static void xdkAudioDestroy(AudioSystem* audio) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    if (arr) {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active) destroyInstance(&arr->instances[i]);
        }
        free(arr);
    }

    if (xa->pMasterVoice) ((IXAudio2MasteringVoice*)xa->pMasterVoice)->DestroyVoice();
    if (xa->pXAudio2) ((IXAudio2*)xa->pXAudio2)->Release();

    for (int i = 0; i < XDK_AUDIO_MAX_CACHED_SOUNDS; i++) {
        if (gSoundCache[i].valid && gSoundCache[i].pcmData) {
            free(gSoundCache[i].pcmData);
        }
        memset(&gSoundCache[i], 0, sizeof(gSoundCache[i]));
    }
    gSoundCacheBytes = 0;

    if (xa->base.audioGroups) {
        for (int32_t i = 1; i < (int32_t)arrlen(xa->base.audioGroups); i++) {
            if (xa->base.audioGroups[i]) DataWin_free(xa->base.audioGroups[i]);
        }
        arrfree(xa->base.audioGroups);
    }
    free(xa);
}

static void xdkAudioUpdate(AudioSystem* audio, float deltaTime) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized) return;
    XdkInstanceArray* arr = Instances(xa);

    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;

        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->currentGain = inst->targetGain;
                inst->fadeTimeRemaining = 0.0f;
            } else if (inst->fadeTotalTime > 0.0f) {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            if (inst->pVoice) {
                inst->pVoice->SetVolume(inst->currentGain * inst->soundVolume * xa->masterGain);
            }
        }

        if (inst->streaming && !inst->paused) {
            submitStreamingBuffers(inst, false);
        }

        if (!inst->loop && !instanceLooksPlaying(inst)) {
            DataWin* dw = getAudioGroup(xa, 0);
            const char* stoppedName = NULL;
            if (dw && inst->soundIndex >= 0 && (uint32_t)inst->soundIndex < dw->sond.count) {
                stoppedName = soundAtSafe(dw, inst->soundIndex)->name;
            }
            if (inst->music) {
                audioTrace(true, "AUD2: finished music idx=%d name=%s instance=%d",
                    inst->soundIndex, stoppedName ? stoppedName : "?", inst->instanceId);
            }
            destroyInstance(inst);
            maybeResumeSuspendedRoomMusic(audio, stoppedName);
        }
    }
}

static int32_t xdkPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized) return -1;

    if (soundIndex >= XDK_STREAM_HANDLE_BASE && soundIndex < XDK_STREAM_HANDLE_BASE + XDK_MAX_STREAMS) {
        int32_t st = soundIndex - XDK_STREAM_HANDLE_BASE;
        if (!gStreamUsed[st]) return -1;
        XdkSoundInstance* sinst = findFreeSlot(xa);
        if (!sinst) {
            audioTrace(true, "AUD2: no free slot for stream handle=%d", soundIndex);
            return -1;
        }
        memset(sinst, 0, sizeof(XdkSoundInstance));
        int32_t sslot = (int32_t)(sinst - Instances(xa)->instances);
        sinst->active = true;
        sinst->soundIndex = soundIndex;
        sinst->instanceId = XDK_SOUND_INSTANCE_ID_BASE + sslot;
        sinst->priority = priority;
        sinst->loop = loop;
        sinst->music = true;
        sinst->streaming = true;
        sinst->startedTick = nowMs();
        sinst->currentGain = 1.0f;
        sinst->targetGain = 1.0f;
        sinst->startGain = 1.0f;
        sinst->pitch = 1.0f;
        sinst->soundVolume = 1.0f;
        sinst->soundPitch = 1.0f;
        xa->nextInstanceCounter++;
        bool sok = createStreamingVoiceForInstance(xa, sinst, gStreamPaths[st], true);
        audioTrace(true, "AUD2: play stream handle=%d loop=%d path=%s ok=%d", soundIndex, loop ? 1 : 0, gStreamPaths[st], sok ? 1 : 0);
        if (!sok) {
            destroyInstance(sinst);
            return -1;
        }
        return sinst->instanceId;
    }

    DataWin* dw = getAudioGroup(xa, 0);
    if (!dw || soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) return -1;



    Sound* sound = soundAtSafe(dw, soundIndex);
    bool isMusic = isMusicSoundName(sound->name);
    static bool tracedSounds[XDK_AUDIO_MAX_CACHED_SOUNDS];
    bool traceThisSound = false;
    if (soundIndex >= 0 && soundIndex < XDK_AUDIO_MAX_CACHED_SOUNDS && !tracedSounds[soundIndex]) {
        tracedSounds[soundIndex] = true;
        traceThisSound = true;
    }
    if (isMusic) {
        traceThisSound = true;
    }

    if (isMusic) {
        if (!gResumingSuspendedMusic && gAwaitingBattleExitMusicIndex >= 0 && soundIndex != gAwaitingBattleExitMusicIndex) {
            audioTrace(true, "AUD2: cancel battle-exit music resume idx=%d because new music idx=%d name=%s",
                gAwaitingBattleExitMusicIndex, soundIndex, sound->name ? sound->name : "?");
            gAwaitingBattleExitMusicIndex = -1;
            gAwaitingBattleExitMusicLoop = true;
        }

        XdkInstanceArray* arr = Instances(xa);
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            XdkSoundInstance* active = &arr->instances[i];
            if (!active->active) continue;
            if (active->soundIndex == soundIndex) {
                audioTrace(true, "AUD2: reuse active music idx=%d name=%s instance=%d",
                    soundIndex, sound->name ? sound->name : "?", active->instanceId);
                return active->instanceId;
            }
            Sound* activeSound = soundAtSafe(dw, active->soundIndex);
            if (isMusicSoundName(activeSound->name)) {
                if (canLayerMusicSoundNames(activeSound->name, sound->name)) {
                    audioTrace(true, "AUD2: keep layered music oldIdx=%d oldName=%s newIdx=%d newName=%s",
                        active->soundIndex, activeSound->name ? activeSound->name : "?",
                        soundIndex, sound->name ? sound->name : "?");
                    continue;
                }
                if (active->loop && !loop && !isBattleMusicSoundName(sound->name)) {
                    audioTrace(true, "AUD2: keep looped music under stinger oldIdx=%d oldName=%s newIdx=%d newName=%s",
                        active->soundIndex, activeSound->name ? activeSound->name : "?",
                        soundIndex, sound->name ? sound->name : "?");
                    continue;
                }
                rememberSuspendedRoomMusic(active, activeSound, sound);
                audioTrace(true, "AUD2: pre-stop music oldIdx=%d oldName=%s newIdx=%d newName=%s",
                    active->soundIndex, activeSound->name ? activeSound->name : "?",
                    soundIndex, sound->name ? sound->name : "?");
                destroyInstance(active);
            }
        }
    }

    if (shouldStreamExternalMusic(sound)) {
        char* path = resolveExternalPath(xa, sound);
        if (!path) {
            audioTrace(true, "AUD2: stream no external path idx=%d name=%s", soundIndex, sound->name ? sound->name : "?");
            return -1;
        }

        XdkSoundInstance* inst = findFreeSlot(xa);
        if (!inst) {
            audioTrace(true, "AUD2: no free stream slot idx=%d name=%s", soundIndex, sound->name ? sound->name : "?");
            free(path);
            return -1;
        }

        memset(inst, 0, sizeof(XdkSoundInstance));
        int32_t slotIndex = (int32_t)(inst - Instances(xa)->instances);
        inst->active = true;
        inst->soundIndex = soundIndex;
        inst->instanceId = XDK_SOUND_INSTANCE_ID_BASE + slotIndex;
        inst->priority = priority;
        inst->loop = loop;
        inst->music = isMusic;
        inst->streaming = true;
        inst->startedTick = nowMs();
        inst->currentGain = sound->volume >= 0.0f ? sound->volume : 1.0f;
        inst->targetGain = inst->currentGain;
        inst->startGain = inst->currentGain;
        inst->pitch = 1.0f;
        inst->soundVolume = 1.0f;
        inst->soundPitch = sound->pitch > 0.0f ? sound->pitch : 1.0f;
        xa->nextInstanceCounter++;

        bool ok = createStreamingVoiceForInstance(xa, inst, path, traceThisSound);
        free(path);
        if (!ok) {
            destroyInstance(inst);
            return -1;
        }
        return inst->instanceId;
    }

    bool ownsDecoded = false;
    XdkDecodedSound* decoded = decodeSoundCached(xa, soundIndex, traceThisSound, &ownsDecoded);
    if (!decoded) return -1;

    XdkSoundInstance* inst = findFreeSlot(xa);
    if (!inst) {
        audioTrace(true, "AUD2: no free slot idx=%d name=%s", soundIndex, sound->name ? sound->name : "?");
        return -1;
    }

    memset(inst, 0, sizeof(XdkSoundInstance));
    int32_t slotIndex = (int32_t)(inst - Instances(xa)->instances);
    inst->active = true;
    inst->soundIndex = soundIndex;
    inst->instanceId = XDK_SOUND_INSTANCE_ID_BASE + slotIndex;
    inst->priority = priority;
    inst->loop = loop;
    inst->decoded = decoded;
    inst->ownsDecoded = ownsDecoded;
    inst->music = isMusic;
    inst->startedTick = nowMs();
    inst->currentGain = sound->volume >= 0.0f ? sound->volume : 1.0f;
    inst->targetGain = inst->currentGain;
    inst->startGain = inst->currentGain;
    inst->pitch = 1.0f;
    inst->soundVolume = 1.0f;
    inst->soundPitch = sound->pitch > 0.0f ? sound->pitch : 1.0f;
    xa->nextInstanceCounter++;

    if (!createVoiceForInstance(xa, inst, traceThisSound)) {
        destroyInstance(inst);
        return -1;
    }

    return inst->instanceId;
}

static void xdkStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    if (isInstanceId(soundOrInstance)) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) {
            DataWin* dw = getAudioGroup(xa, 0);
            const char* stoppedName = NULL;
            if (dw && inst->soundIndex >= 0 && (uint32_t)inst->soundIndex < dw->sond.count) {
                stoppedName = soundAtSafe(dw, inst->soundIndex)->name;
            }
            bool stoppedMusic = inst->music;
            audioTrace(stoppedMusic, "AUD2: stop instance=%d idx=%d name=%s music=%d",
                soundOrInstance, inst->soundIndex, stoppedName ? stoppedName : "?", stoppedMusic ? 1 : 0);
            destroyInstance(inst);
            maybeResumeSuspendedRoomMusic(audio, stoppedName);
        }
        return;
    }

    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
            DataWin* dw = getAudioGroup(xa, 0);
            const char* stoppedName = NULL;
            if (dw && soundOrInstance >= 0 && (uint32_t)soundOrInstance < dw->sond.count) {
                stoppedName = soundAtSafe(dw, soundOrInstance)->name;
            }
            bool stoppedMusic = arr->instances[i].music;
            audioTrace(stoppedMusic, "AUD2: stop sound idx=%d name=%s instance=%d music=%d",
                soundOrInstance, stoppedName ? stoppedName : "?", arr->instances[i].instanceId, stoppedMusic ? 1 : 0);
            destroyInstance(&arr->instances[i]);
            maybeResumeSuspendedRoomMusic(audio, stoppedName);
        }
    }
}

static void xdkStopAll(AudioSystem* audio) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    DataWin* dw = getAudioGroup(xa, 0);
    const char* stoppedBattleName = NULL;
    audioTrace(true, "AUD2: stop all");
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) {
            XdkSoundInstance* inst = &arr->instances[i];
            const char* stoppedName = NULL;
            if (dw && inst->soundIndex >= 0 && (uint32_t)inst->soundIndex < dw->sond.count) {
                stoppedName = soundAtSafe(dw, inst->soundIndex)->name;
            }
            if (inst->music) {
                audioTrace(true, "AUD2: stop all music idx=%d name=%s instance=%d",
                    inst->soundIndex, stoppedName ? stoppedName : "?", inst->instanceId);
                if (!stoppedBattleName && isBattleMusicSoundName(stoppedName)) {
                    stoppedBattleName = stoppedName;
                }
            }
            destroyInstance(inst);
        }
    }
    maybeResumeSuspendedRoomMusic(audio, stoppedBattleName);
}

static bool xdkIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    if (isInstanceId(soundOrInstance)) {
        return instanceLooksPlaying(findById(xa, soundOrInstance));
    }

    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance && instanceLooksPlaying(&arr->instances[i])) {
            return true;
        }
    }
    return false;
}

static void xdkPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            if (!inst->paused) {
                inst->paused = true;
                inst->pauseStartedTick = nowMs();
                if (inst->pVoice) inst->pVoice->Stop(0);
            }
        }
    }
}

static void xdkResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            if (inst->paused) {
                inst->pausedTotalMs += nowMs() - inst->pauseStartedTick;
                inst->paused = false;
                if (inst->pVoice) inst->pVoice->Start(0);
            }
        }
    }
}

static void xdkPauseAll(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) xdkPauseSound(audio, arr->instances[i].instanceId);
    }
}

static void xdkResumeAll(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) xdkResumeSound(audio, arr->instances[i].instanceId);
    }
}

static void xdkSuspend(AudioSystem* audio) {
    xdkPauseAll(audio);
}

static void xdkResumeSystem(AudioSystem* audio) {
    xdkResumeAll(audio);
}

static void xdkSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                if (inst->pVoice) inst->pVoice->SetVolume(gain * inst->soundVolume * xa->masterGain);
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float)timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    }
}

static float xdkGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            return inst->currentGain;
        }
    }
    return 0.0f;
}

static void xdkSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            inst->pitch = pitch > 0.0f ? pitch : 1.0f;
            if (inst->pVoice) inst->pVoice->SetFrequencyRatio(inst->pitch * inst->soundPitch);
        }
    }
}

static float xdkGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            return inst->pitch;
        }
    }
    return 1.0f;
}

static float xdkGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            return (float)instanceElapsedMs(inst) / 1000.0f;
        }
    }
    return 0.0f;
}

static void xdkSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active || !inst->pVoice) continue;
        if ((isInstanceId(soundOrInstance) && inst->instanceId == soundOrInstance) ||
            (!isInstanceId(soundOrInstance) && inst->soundIndex == soundOrInstance)) {
            if (inst->streaming) {
                uint32_t frame = (uint32_t)(positionSeconds * (float)inst->streamSampleRate);
                if (inst->streamTotalFrames > 0 && frame >= inst->streamTotalFrames) frame = 0;
                inst->pVoice->Stop(0);
                inst->pVoice->FlushSourceBuffers();
                if (!stb_vorbis_seek(inst->streamVorbis, frame)) {
                    stb_vorbis_seek_start(inst->streamVorbis);
                    frame = 0;
                }
                inst->streamSubmitted = 0;
                inst->streamEof = false;
                submitStreamingBuffers(inst, false);
                inst->startedTick = nowMs() - (uint32_t)(positionSeconds * 1000.0f);
                inst->pausedTotalMs = 0;
                if (!inst->paused) inst->pVoice->Start(0);
                continue;
            }

            if (!inst->decoded) continue;
            uint32_t frame = (uint32_t)(positionSeconds * (float)inst->decoded->sampleRate);
            if (frame >= inst->decoded->sampleFrames) frame = 0;
            inst->pVoice->Stop(0);
            inst->pVoice->FlushSourceBuffers();
            submitFromFrame(inst, frame);
            inst->startedTick = nowMs() - (uint32_t)(positionSeconds * 1000.0f);
            inst->pausedTotalMs = 0;
            if (!inst->paused) inst->pVoice->Start(0);
        }
    }
}

static float xdkGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    if (isInstanceId(soundOrInstance)) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst ? instanceDurationSeconds(inst) : 0.0f;
    }

    DataWin* dw = getAudioGroup(xa, 0);
    if (dw && soundOrInstance >= 0 && (uint32_t)soundOrInstance < dw->sond.count) {
        Sound* sound = soundAtSafe(dw, soundOrInstance);
        if (shouldStreamExternalMusic(sound)) {
            char* path = resolveExternalPath(xa, sound);
            if (path) {
                int err = 0;
                stb_vorbis* vorbis = stb_vorbis_open_filename(path, &err, NULL);
                free(path);
                if (vorbis) {
                    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
                    int frames = stb_vorbis_stream_length_in_samples(vorbis);
                    stb_vorbis_close(vorbis);
                    if (info.sample_rate > 0 && frames > 0) {
                        return (float)frames / (float)info.sample_rate;
                    }
                }
            }
        }
    }

    bool ownsDecoded = false;
    XdkDecodedSound* decoded = decodeSoundCached(xa, soundOrInstance, false, &ownsDecoded);
    float seconds = decodedDurationSeconds(decoded);
    if (ownsDecoded && decoded) {
        free(decoded->pcmData);
        free(decoded);
    }
    return seconds;
}

static void xdkSetMasterGain(AudioSystem* audio, float gain) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    xa->masterGain = gain;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->pVoice) {
            inst->pVoice->SetVolume(inst->currentGain * inst->soundVolume * xa->masterGain);
        }
    }
}

static void xdkSetChannelCount(AudioSystem* audio, int32_t count) {
    (void)audio;
    (void)count;
}

static void xdkGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (groupIndex <= 0 || !xa->fileSystem) return;
    if (groupIndex < (int32_t)arrlen(xa->base.audioGroups) && xa->base.audioGroups[groupIndex]) return;

    char filename[64];
    _snprintf(filename, sizeof(filename), "audiogroup%d.dat", groupIndex);
    filename[sizeof(filename) - 1] = '\0';
    char* path = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
    Butterscotch_xdkDiagTrace("AUD2: stream path '%s'", path);
    if (!path) return;

    DataWinParserOptions options;
    memset(&options, 0, sizeof(options));
    options.parseGen8 = true;
    options.parseOptn = true;
    options.parseSond = true;
    options.parseAgrp = true;
    options.parseAudo = true;
    options.loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
    DataWin* group = DataWin_parse(path, options);
    free(path);
    if (!group) return;

    while ((int32_t)arrlen(xa->base.audioGroups) <= groupIndex) {
        arrput(xa->base.audioGroups, (DataWin*)NULL);
    }
    xa->base.audioGroups[groupIndex] = group;
    audioTrace(true, "AUD2: loaded audio group %d", groupIndex);
}

static bool xdkGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    return groupIndex >= 0 && groupIndex < (int32_t)arrlen(xa->base.audioGroups) && xa->base.audioGroups[groupIndex] != NULL;
}

static int32_t xdkCreateStream(AudioSystem* audio, const char* filename) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized || !filename || !filename[0]) return -1;
    if (!xa->fileSystem->vtable->fileExists(xa->fileSystem, filename)) {
        audioTrace(true, "AUD2: createStream missing file %s", filename);
        return -1;
    }
    char* path = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
    if (!path) return -1;
    for (int32_t i = 0; i < XDK_MAX_STREAMS; i++) {
        if (!gStreamUsed[i]) {
            strncpy(gStreamPaths[i], path, sizeof(gStreamPaths[i]) - 1);
            gStreamPaths[i][sizeof(gStreamPaths[i]) - 1] = '\0';
            gStreamUsed[i] = true;
            free(path);
            audioTrace(true, "AUD2: createStream handle=%d path=%s", XDK_STREAM_HANDLE_BASE + i, gStreamPaths[i]);
            return XDK_STREAM_HANDLE_BASE + i;
        }
    }
    free(path);
    audioTrace(true, "AUD2: createStream no free slot");
    return -1;
}

static bool xdkDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    int32_t st = streamIndex - XDK_STREAM_HANDLE_BASE;
    if (st < 0 || st >= XDK_MAX_STREAMS || !gStreamUsed[st]) return false;
    XdkInstanceArray* arr = Instances(xa);
    int32_t stopped = 0;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->streaming && inst->soundIndex == streamIndex) {
            destroyInstance(inst);
            stopped++;
        }
    }
    audioTrace(true, "AUD2: destroyStream handle=%d stoppedInstances=%d", streamIndex, stopped);
    gStreamUsed[st] = false;
    return true;
}

static AudioSystemVtable xdkAudioVtable = {};

XdkAudioSystem* XdkAudioSystem_create(void) {
    XdkAudioSystem* xa = (XdkAudioSystem*)calloc(1, sizeof(XdkAudioSystem));
    xdkAudioVtable.init = xdkAudioInit;
    xdkAudioVtable.destroy = xdkAudioDestroy;
    xdkAudioVtable.update = xdkAudioUpdate;
    xdkAudioVtable.playSound = xdkPlaySound;
    xdkAudioVtable.stopSound = xdkStopSound;
    xdkAudioVtable.stopAll = xdkStopAll;
    xdkAudioVtable.isPlaying = xdkIsPlaying;
    xdkAudioVtable.pauseSound = xdkPauseSound;
    xdkAudioVtable.resumeSound = xdkResumeSound;
    xdkAudioVtable.pauseAll = xdkPauseAll;
    xdkAudioVtable.resumeAll = xdkResumeAll;
    xdkAudioVtable.suspend = xdkSuspend;
    xdkAudioVtable.resume = xdkResumeSystem;
    xdkAudioVtable.setSoundGain = xdkSetSoundGain;
    xdkAudioVtable.getSoundGain = xdkGetSoundGain;
    xdkAudioVtable.setSoundPitch = xdkSetSoundPitch;
    xdkAudioVtable.getSoundPitch = xdkGetSoundPitch;
    xdkAudioVtable.getTrackPosition = xdkGetTrackPosition;
    xdkAudioVtable.setTrackPosition = xdkSetTrackPosition;
    xdkAudioVtable.getSoundLength = xdkGetSoundLength;
    xdkAudioVtable.setMasterGain = xdkSetMasterGain;
    xdkAudioVtable.setChannelCount = xdkSetChannelCount;
    xdkAudioVtable.groupLoad = xdkGroupLoad;
    xdkAudioVtable.groupIsLoaded = xdkGroupIsLoaded;
    xdkAudioVtable.createStream = xdkCreateStream;
    xdkAudioVtable.destroyStream = xdkDestroyStream;
    xa->base.vtable = &xdkAudioVtable;
    xa->masterGain = 1.0f;
    xa->instanceData = calloc(1, sizeof(XdkInstanceArray));
    return xa;
}
