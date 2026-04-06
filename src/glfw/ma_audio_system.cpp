// On Windows, include windows.h first so its headers are processed before stb_vorbis
// defines single-letter macros (L, C, R) that conflict with winnt.h struct field names.
#ifdef _WIN32
#include <windows.h>
#endif

// Include stb_vorbis BEFORE miniaudio so that STB_VORBIS_INCLUDE_STB_VORBIS_H is defined,
// which enables miniaudio's built-in OGG Vorbis decoding support.
#ifndef STB_VORBIS_INCLUDE_STB_VORBIS_H
#define STB_VORBIS_IMPLEMENTATION
#include "../../vendor/stb/vorbis/stb_vorbis.c"
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ma_audio_system.h"
#include "data_win.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stb_ds.h"

// ===[ Helpers ]===

// ===[ CachedSound ma_data_source vtable ]===
static ma_result cachedSoundReadPCMFrames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead);
static ma_result cachedSoundSeek(ma_data_source* pDataSource, ma_uint64 frameIndex);
static ma_result cachedSoundTell(ma_data_source* pDataSource, ma_uint64* pCursor);
static ma_result cachedSoundLength(ma_data_source* pDataSource, ma_uint64* pLength);

static ma_data_source_vtable g_cachedSoundVtable = {
    cachedSoundReadPCMFrames,
    cachedSoundSeek,
    cachedSoundTell,
    cachedSoundLength,
    nullptr,
    0
};

// Check if sound name contains "TXT" (frequently played sounds)
static bool isFrequentSound(const char* name) {
    if (!name) return false;
    // Case-insensitive search for "txt" in the name
    for (const char* p = name; *p; p++) {
        if ((p[0] == 'T' || p[0] == 't') &&
            (p[1] == 'X' || p[1] == 'x') &&
            (p[2] == 'T' || p[2] == 't')) {
            return true;
        }
    }
    return false;
}

// Find cached sound by index, returns nullptr if not found
static CachedSound* findCachedSound(MaAudioSystem* ma, int32_t soundIndex) {
    repeat(MAX_CACHED_SOUNDS, i) {
        if (ma->cachedSounds[i].soundIndex == soundIndex && ma->cachedSounds[i].pcmData) {
            return &ma->cachedSounds[i];
        }
    }
    return nullptr;
}

// Find free cache slot
static CachedSound* findFreeCacheSlot(MaAudioSystem* ma) {
    repeat(MAX_CACHED_SOUNDS, i) {
        if (!ma->cachedSounds[i].pcmData) return &ma->cachedSounds[i];
    }
    // Cache full — evict oldest (index 0)
    CachedSound* slot = &ma->cachedSounds[0];
    ma_free(slot->pcmData, nullptr);
    slot->pcmData = nullptr;
    slot->frameCount = 0;
    return slot;
}

// Decode sound to PCM and cache it
static bool cacheSound(MaAudioSystem* ma, Sound* sound) {
    if (!sound) return false;

    // Resolve file path
    const char* file = sound->file;
    if (!file || file[0] == '\0') return false;

    bool hasExtension = (strchr(file, '.') != nullptr);
    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }

    char* resolvedPath = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    if (!resolvedPath) return false;

    // Decode to PCM (mono, f32)
    ma_decoder decoder;
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 1, 44100);
    ma_result result = ma_decoder_init_file(resolvedPath, &decoderConfig, &decoder);
    free(resolvedPath);
    if (result != MA_SUCCESS) return false;

    // Read all PCM frames
    ma_uint64 framesRead = 0;
    ma_uint64 totalFrames = 0;
    float* pcmBuffer = nullptr;

    // First pass: count frames
    float tempBuf[4096];
    while (true) {
        ma_uint64 frames;
        result = ma_decoder_read_pcm_frames(&decoder, tempBuf, 4096, &frames);
        if (result != MA_SUCCESS || frames == 0) break;
        totalFrames += frames;
    }

    if (totalFrames == 0) {
        ma_decoder_uninit(&decoder);
        return false;
    }

    // Second pass: read into buffer
    pcmBuffer = (float*)ma_malloc(totalFrames * sizeof(float), nullptr);
    if (!pcmBuffer) {
        ma_decoder_uninit(&decoder);
        return false;
    }

    // Rewind and read
    ma_decoder_seek_to_pcm_frame(&decoder, 0);
    result = ma_decoder_read_pcm_frames(&decoder, pcmBuffer, totalFrames, &framesRead);
    ma_decoder_uninit(&decoder);

    if (result != MA_SUCCESS) {
        ma_free(pcmBuffer, nullptr);
        return false;
    }

    // Store in cache
    CachedSound* slot = findFreeCacheSlot(ma);
    slot->vtable = &g_cachedSoundVtable;
    slot->soundIndex = sound->audioFile;
    slot->pcmData = pcmBuffer;
    slot->frameCount = framesRead;
    slot->sampleRate = decoder.outputSampleRate;
    slot->cursorFrame = 0;

    fprintf(stderr, "Audio: Cached sound '%s' (%llu frames, mono f32)\n", sound->name, (unsigned long long)framesRead);
    return true;
}

// ===[ CachedSound ma_data_source vtable ]===
static ma_result cachedSoundReadPCMFrames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
    CachedSound* cached = (CachedSound*)pDataSource;
    float* out = (float*)pFramesOut;
    ma_uint64 available = cached->frameCount - cached->cursorFrame;
    ma_uint64 toRead = frameCount < available ? frameCount : available;
    for (ma_uint64 i = 0; i < toRead; i++) {
        out[i] = cached->pcmData[cached->cursorFrame + i];
    }
    cached->cursorFrame += toRead;
    if (pFramesRead) *pFramesRead = toRead;
    return toRead > 0 ? MA_SUCCESS : MA_AT_END;
}

static ma_result cachedSoundSeek(ma_data_source* pDataSource, ma_uint64 frameIndex) {
    CachedSound* cached = (CachedSound*)pDataSource;
    if (frameIndex > cached->frameCount) return MA_INVALID_ARGS;
    cached->cursorFrame = frameIndex;
    return MA_SUCCESS;
}

static ma_result cachedSoundTell(ma_data_source* pDataSource, ma_uint64* pCursor) {
    CachedSound* cached = (CachedSound*)pDataSource;
    if (pCursor) *pCursor = cached->cursorFrame;
    return MA_SUCCESS;
}

static ma_result cachedSoundLength(ma_data_source* pDataSource, ma_uint64* pLength) {
    CachedSound* cached = (CachedSound*)pDataSource;
    if (pLength) *pLength = cached->frameCount;
    return MA_SUCCESS;
}

static SoundInstance* findFreeSlot(MaAudioSystem* ma) {
    // First pass: find an inactive slot
    repeat(MAX_SOUND_INSTANCES, i) {
        if (!ma->instances[i].active) {
            return &ma->instances[i];
        }
    }

    // Second pass: evict the lowest-priority ended sound
    SoundInstance* best = nullptr;
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!ma_sound_is_playing(&inst->maSound)) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        ma_sound_uninit(&best->maSound);
        if (best->ownsDecoder) {
            ma_decoder_uninit(&best->decoder);
        }
        best->active = false;
    }

    return best;
}

static SoundInstance* findInstanceById(MaAudioSystem* ma, int32_t instanceId) {
    int32_t slotIndex = instanceId - SOUND_INSTANCE_ID_BASE;
    if (0 > slotIndex || slotIndex >= MAX_SOUND_INSTANCES) return nullptr;
    SoundInstance* inst = &ma->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}

// Helper: resolve external audio file path from Sound entry
static char* resolveExternalPath(MaAudioSystem* ma, Sound* sound) {
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    // If the filename has no extension, append ".ogg"
    bool hasExtension = (strchr(file, '.') != nullptr);

    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }

    char* resolvedPath = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    fprintf(stderr, "Audio: Resolved '%s' -> '%s'\n", file, resolvedPath ? resolvedPath : "NULL");
    return resolvedPath;
}

// ===[ Vtable Implementations ]===

static void maInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    arrput(ma->base.audioGroups, dataWin);
    ma->fileSystem = fileSystem;

    // Configure miniaudio - ALSA only, mono channel (saves 50% CPU and memory)
    ma_engine_config config = ma_engine_config_init();
    config.sampleRate = 44100;
    config.channels = 1; // Mono — half the CPU and memory of stereo

    ma_result result = ma_engine_init(&config, &ma->engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Audio: Failed to initialize miniaudio engine (error %d)\n", result);
        return;
    }

    fprintf(stderr, "Audio: miniaudio engine initialized (sample rate: %u, channels: %u, mono)\n",
            config.sampleRate, config.channels);

    memset(ma->instances, 0, sizeof(ma->instances));
    ma->nextInstanceCounter = 0;
}

static void maDestroy(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Uninit all active sound instances
    repeat(MAX_SOUND_INSTANCES, i) {
        if (ma->instances[i].active) {
            ma_sound_uninit(&ma->instances[i].maSound);
            if (ma->instances[i].ownsDecoder) {
                ma_decoder_uninit(&ma->instances[i].decoder);
            }
            ma->instances[i].active = false;
        }
    }

    // Free stream entries
    repeat(MAX_AUDIO_STREAMS, i) {
        if (ma->streams[i].active) {
            free(ma->streams[i].filePath);
        }
    }

    // Free cached PCM data
    repeat(MAX_CACHED_SOUNDS, i) {
        if (ma->cachedSounds[i].pcmData) {
            ma_free(ma->cachedSounds[i].pcmData, nullptr);
            ma->cachedSounds[i].pcmData = nullptr;
        }
    }

    ma_engine_uninit(&ma->engine);
    free(ma);
}

static void maUpdate(AudioSystem* audio, float deltaTime) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!inst->active) continue;

        // Handle gain fading (for cases where we do manual fading)
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (0.0f >= inst->fadeTimeRemaining) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            ma_sound_set_volume(&inst->maSound, inst->currentGain);
        }

        // Clean up ended non-looping sounds (ma_sound_at_end avoids reaping still-loading async sounds)
        if (ma_sound_at_end(&inst->maSound) && !ma_sound_is_looping(&inst->maSound)) {
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }
}

static int32_t maPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Check if this is a stream index (created by audio_create_stream)
    bool isStream = (soundIndex >= AUDIO_STREAM_INDEX_BASE);
    Sound* sound = nullptr;
    char* streamPath = nullptr;

    if (isStream) {
        int32_t streamSlot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (0 > streamSlot || streamSlot >= MAX_AUDIO_STREAMS || !ma->streams[streamSlot].active) {
            fprintf(stderr, "Audio: Invalid stream index %d\n", soundIndex);
            return -1;
        }
        streamPath = ma->streams[streamSlot].filePath;
    } else {
        DataWin* dw = ma->base.audioGroups[0]; // Audio Group 0 should always be data.win
        if (0 > soundIndex || (uint32_t) soundIndex >= dw->sond.count) {
            fprintf(stderr, "Audio: Invalid sound index %d\n", soundIndex);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
    }

    SoundInstance* slot = findFreeSlot(ma);
    if (slot == nullptr) {
        fprintf(stderr, "Audio: No free sound slots for sound %d\n", soundIndex);
        return -1;
    }

    int32_t slotIndex = (int32_t) (slot - ma->instances);
    ma_result result;

    if (isStream) {
        // Stream audio: load from file path stored in stream entry
        result = ma_sound_init_from_file(&ma->engine, streamPath, MA_SOUND_FLAG_ASYNC, nullptr, nullptr, &slot->maSound);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to load stream file '%s' (error %d)\n", streamPath, result);
            return -1;
        }
        slot->ownsDecoder = false;
    } else {
        bool isEmbedded = (sound->flags & 0x01) != 0;

        if (isEmbedded) {
            // Embedded audio: decode from AUDO chunk memory
            if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) {
                fprintf(stderr, "Audio: Invalid audio file index %d for sound '%s'\n", sound->audioFile, sound->name);
                return -1;
            }

            // Check cache first for frequent sounds (TXT)
            CachedSound* cached = nullptr;
            if (isFrequentSound(sound->name)) {
                cached = findCachedSound(ma, sound->audioFile);
                if (cached) {
                    // Reset cursor to beginning
                    cached->cursorFrame = 0;
                    // Play from cached PCM (CachedSound layout matches ma_data_source)
                    result = ma_sound_init_from_data_source(&ma->engine, (ma_data_source*)cached, 0, nullptr, &slot->maSound);
                    if (result == MA_SUCCESS) {
                        slot->ownsDecoder = false;
                        slot->soundIndex = soundIndex;
                        slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
                        slot->priority = priority;
                        slot->targetGain = sound->volume;
                        slot->currentGain = sound->volume;
                        slot->fadeTimeRemaining = 0;
                        slot->startGain = sound->volume;
                        ma_sound_set_volume(&slot->maSound, slot->currentGain);
                        if (sound->pitch != 1.0f) {
                            ma_sound_set_pitch(&slot->maSound, sound->pitch);
                        }
                        ma_sound_set_looping(&slot->maSound, loop);
                        ma_sound_start(&slot->maSound);
                        slot->active = true;
                        return slot->instanceId;
                    }
                }
            }

            AudioEntry* entry = &ma->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];

            ma_decoder_config decoderConfig = ma_decoder_config_init_default();
            result = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &slot->decoder);
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to init decoder for '%s' (error %d)\n", sound->name, result);
                return -1;
            }
            slot->ownsDecoder = true;

            // NOTE: Embedded sound caching removed — ma_decoder_init_memory causes SIGFPE on ARMv5
            // and is redundant since miniaudio already decodes them on first play.

            result = ma_sound_init_from_data_source(&ma->engine, &slot->decoder, 0, nullptr, &slot->maSound);
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to init sound from decoder for '%s' (error %d)\n", sound->name, result);
                ma_decoder_uninit(&slot->decoder);
                return -1;
            }
        } else {
            // External audio: load from file
            char* path = resolveExternalPath(ma, sound);
            if (path == nullptr) {
                fprintf(stderr, "Audio: Could not resolve path for sound '%s'\n", sound->name);
                return -1;
            }

            result = ma_sound_init_from_file(&ma->engine, path, MA_SOUND_FLAG_ASYNC, nullptr, nullptr, &slot->maSound);
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to load file for '%s' at '%s' (error %d)\n", sound->name, path, result);
                free(path);
                return -1;
            }
            free(path);
            slot->ownsDecoder = false;
        }
    }

    // Apply properties
    float volume = isStream ? 1.0f : sound->volume;
    float pitch = isStream ? 1.0f : sound->pitch;
    ma_sound_set_volume(&slot->maSound, volume);
    if (pitch != 1.0f) {
        ma_sound_set_pitch(&slot->maSound, pitch);
    }
    ma_sound_set_looping(&slot->maSound, loop);

    // Set up instance tracking
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->startGain = volume;
    slot->priority = priority;

    // Track unique IDs for disambiguation
    ma->nextInstanceCounter++;

    ma_sound_start(&slot->maSound);

    return slot->instanceId;
}

static void maStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        // Stop specific instance
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    } else {
        // Stop all instances of this sound resource
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
                ma_sound_uninit(&inst->maSound);
                if (inst->ownsDecoder) {
                    ma_decoder_uninit(&inst->decoder);
                }
                inst->active = false;
            }
        }
    }
}

static void maStopAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }
}

static bool maIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        return inst != nullptr && ma_sound_is_playing(&inst->maSound);
    } else {
        // Check if any instance of this sound resource is playing
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance && ma_sound_is_playing(&inst->maSound)) {
                return true;
            }
        }
        return false;
    }
}

static void maPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
            }
        }
    }
}

static void maResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_start(&inst->maSound);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_start(&inst->maSound);
            }
        }
    }
}

static void maPauseAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && ma_sound_is_playing(&inst->maSound)) {
            ma_sound_stop(&inst->maSound);
        }
    }
}

static void maResumeAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            ma_sound_start(&inst->maSound);
        }
    }
}

static void maSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                ma_sound_set_volume(&inst->maSound, gain);
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float) timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (timeMs == 0) {
                    inst->currentGain = gain;
                    inst->targetGain = gain;
                    inst->fadeTimeRemaining = 0.0f;
                    ma_sound_set_volume(&inst->maSound, gain);
                } else {
                    inst->startGain = inst->currentGain;
                    inst->targetGain = gain;
                    inst->fadeTotalTime = (float) timeMs / 1000.0f;
                    inst->fadeTimeRemaining = inst->fadeTotalTime;
                }
            }
        }
    }
}

static float maGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return inst->currentGain;
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return inst->currentGain;
            }
        }
    }
    return 0.0f;
}

static void maSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_set_pitch(&inst->maSound, pitch);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_set_pitch(&inst->maSound, pitch);
            }
        }
    }
}

static float maGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return ma_sound_get_pitch(&inst->maSound);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return ma_sound_get_pitch(&inst->maSound);
            }
        }
    }
    return 1.0f;
}

static float maGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            float cursor;
            ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
            if (result == MA_SUCCESS) return cursor;
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                float cursor;
                ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
                if (result == MA_SUCCESS) return cursor;
            }
        }
    }
    return 0.0f;
}

static void maSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64) (positionSeconds * 44100.0f));
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64) (positionSeconds * 44100.0f));
            }
        }
    }
}

static void maSetMasterGain(AudioSystem* audio, float gain) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    ma_engine_set_volume(&ma->engine, gain);
}

static void maSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {
    // miniaudio handles channel management internally, this is a no-op
}

static void maGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex > 0) {
        int sz = snprintf(nullptr, 0, "audiogroup%d.dat", groupIndex);
        char buf[sz + 1];
        snprintf(buf, sizeof(buf), "audiogroup%d.dat", groupIndex);
        DataWin *audioGroup = DataWin_parse(((MaAudioSystem*)audio)->fileSystem->vtable->resolvePath(((MaAudioSystem*)audio)->fileSystem, buf),
        (DataWinParserOptions) {
            .parseAudo = true,
        });
        arrput(audio->audioGroups, audioGroup);
    }
}

static bool maGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

// ===[ Audio Streams ]===

static int32_t maCreateStream(AudioSystem* audio, const char* filename) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Find a free stream slot
    int32_t freeSlot = -1;
    repeat(MAX_AUDIO_STREAMS, i) {
        if (!ma->streams[i].active) {
            freeSlot = (int32_t) i;
            break;
        }
    }

    if (0 > freeSlot) {
        fprintf(stderr, "Audio: No free stream slots for '%s'\n", filename);
        return -1;
    }

    char* resolved = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    if (resolved == nullptr) {
        fprintf(stderr, "Audio: Could not resolve path for stream '%s'\n", filename);
        return -1;
    }

    ma->streams[freeSlot].active = true;
    ma->streams[freeSlot].filePath = resolved;

    int32_t streamIndex = AUDIO_STREAM_INDEX_BASE + freeSlot;
    fprintf(stderr, "Audio: Created stream %d for '%s' -> '%s'\n", streamIndex, filename, resolved);
    return streamIndex;
}

static bool maDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    int32_t slotIndex = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (0 > slotIndex || slotIndex >= MAX_AUDIO_STREAMS) {
        fprintf(stderr, "Audio: Invalid stream index %d for destroy\n", streamIndex);
        return false;
    }

    AudioStreamEntry* entry = &ma->streams[slotIndex];
    if (!entry->active) return false;

    // Stop all sound instances that were playing this stream
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == streamIndex) {
            ma_sound_stop(&inst->maSound);
            ma_sound_uninit(&inst->maSound);
            if (inst->ownsDecoder) {
                ma_decoder_uninit(&inst->decoder);
            }
            inst->active = false;
        }
    }

    free(entry->filePath);
    entry->filePath = nullptr;
    entry->active = false;
    fprintf(stderr, "Audio: Destroyed stream %d\n", streamIndex);
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable maAudioSystemVtable = {
    .init = maInit,
    .destroy = maDestroy,
    .update = maUpdate,
    .playSound = maPlaySound,
    .stopSound = maStopSound,
    .stopAll = maStopAll,
    .isPlaying = maIsPlaying,
    .pauseSound = maPauseSound,
    .resumeSound = maResumeSound,
    .pauseAll = maPauseAll,
    .resumeAll = maResumeAll,
    .setSoundGain = maSetSoundGain,
    .getSoundGain = maGetSoundGain,
    .setSoundPitch = maSetSoundPitch,
    .getSoundPitch = maGetSoundPitch,
    .getTrackPosition = maGetTrackPosition,
    .setTrackPosition = maSetTrackPosition,
    .setMasterGain = maSetMasterGain,
    .setChannelCount = maSetChannelCount,
    .groupLoad = maGroupLoad,
    .groupIsLoaded = maGroupIsLoaded,
    .createStream = maCreateStream,
    .destroyStream = maDestroyStream,
};

// ===[ Lifecycle ]===

MaAudioSystem* MaAudioSystem_create(void) {
    MaAudioSystem* ma = safeCalloc(1, sizeof(MaAudioSystem));
    ma->base.vtable = &maAudioSystemVtable;
    return ma;
}
