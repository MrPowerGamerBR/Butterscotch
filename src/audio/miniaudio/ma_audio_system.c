// On Windows, include windows.h first so its headers are processed before stb_vorbis
// defines single-letter macros (L, C, R) that conflict with winnt.h struct field names.
#ifdef _WIN32
#include <windows.h>
#endif

// Include stb_vorbis BEFORE miniaudio so that STB_VORBIS_INCLUDE_STB_VORBIS_H is defined,
// which enables miniaudio's built-in OGG Vorbis decoding support.
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "miniaudio.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "ma_audio_system.h"
#include "data_win.h"
#include "utils.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include "stb_ds.h"

// ===[ Helpers ]===

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

static bool isValidSoundInstanceId(int32_t instanceId) {
    return AUDIO_STREAM_INDEX_BASE > instanceId && instanceId >= SOUND_INSTANCE_ID_BASE;
}

static SoundInstance* findInstanceById(MaAudioSystem* ma, int32_t instanceId) {
    for (int32_t i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && inst->instanceId == instanceId)
            return inst;
    }
    return nullptr;
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

    return ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
}

// ===[ Vtable Implementations ]===

static void maInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    arrput(ma->base.audioGroups, dataWin);
    ma->fileSystem = fileSystem;

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.periods           = 3;
    deviceConfig.periodSizeInMilliseconds = 10;
    deviceConfig.dataCallback = ma_engine_data_callback_internal;
    deviceConfig.pUserData    = &ma->engine;
    ma_result deviceResult = ma_device_init(NULL, &deviceConfig, &ma->device);
    if (deviceResult != MA_SUCCESS) {
        logError("Audio: Failed to initialize playback device (error %d)\n", deviceResult);
        return;
    }
    ma_engine_config config = ma_engine_config_init();
    config.pDevice = &ma->device;
    
    ma_result result = ma_engine_init(&config, &ma->engine);
    if (result != MA_SUCCESS) {
        logError("Audio: Failed to initialize miniaudio engine (error %d)\n", result);
        ma_device_uninit(&ma->device);
        return;
    }

    memset(ma->instances, 0, sizeof(ma->instances));
    ma->nextInstanceCounter = 0;

    repeat(MAX_LISTENERS, i) {
        ma_sound_group_init(&ma->engine, 0, NULL, &ma->listenerGroups[i]);
        ma_sound_group_set_volume(&ma->listenerGroups[i], 1.0f);
        ma->listenerGains[i] = 1.0f;
    }

    logInfo("Audio: miniaudio engine initialized\n");
}

static void maDestroy(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_LISTENERS, i) {
        ma_sound_group_uninit(&ma->listenerGroups[i]);
    }

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

    // Free loaded audio groups. The main data.win is owned by the caller, so skip index 0.
    if (arrlen(ma->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(ma->base.audioGroups); i++) {
            DataWin_free(ma->base.audioGroups[i]);
        }
    }
    arrfree(ma->base.audioGroups);

    ma_device_uninit(&ma->device);
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
    float streamPitch = 1.0f;
    float streamGain = 1.0f;

    if (isStream) {
        int32_t streamSlot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (0 > streamSlot || streamSlot >= MAX_AUDIO_STREAMS || !ma->streams[streamSlot].active) {
            logWarn("Audio: Invalid stream index %d\n", soundIndex);
            return -1;
        }
        AudioStreamEntry* stream = &ma->streams[streamSlot];
        streamPath = stream->filePath;
        streamPitch = stream->initialPitch;
        streamGain = stream->initialGain;
    } else {
        DataWin* dw = ma->base.audioGroups[0]; // Audio Group 0 should always be data.win
        if (0 > soundIndex || (uint32_t) soundIndex >= dw->sond.count) {
            logWarn("Audio: Invalid sound index %d\n", soundIndex);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
    }

    SoundInstance* slot = findFreeSlot(ma);
    if (slot == nullptr) {
        logWarn("Audio: No free sound slots for sound %d\n", soundIndex);
        return -1;
    }

    ma_result result;

    if (isStream) {
        // Stream audio: load from file path stored in stream entry
        result = ma_sound_init_from_file(&ma->engine, streamPath, MA_SOUND_FLAG_ASYNC, &ma->listenerGroups[0], nullptr, &slot->maSound);
        if (result != MA_SUCCESS) {
            logWarn("Audio: Failed to load stream file '%s' (error %d)\n", streamPath, result);
            return -1;
        }
        slot->ownsDecoder = false;
    } else {
        bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
        bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
        bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
        bool inAudo = !isRegular || isEmbedded || isCompressed;

        if (inAudo) {
            // Embedded audio: decode from AUDO chunk memory
            if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) {
                logWarn("Audio: Invalid audio file index %d for sound '%s'\n", sound->audioFile, sound->name);
                return -1;
            }

            DataWin* audioGroup = ma->base.audioGroups[sound->audioGroup];
            DataWin_loadAudoIfNeeded(audioGroup, (uint32_t)sound->audioFile);
            AudioEntry* entry = &audioGroup->audo.entries[sound->audioFile];

            ma_decoder_config decoderConfig = ma_decoder_config_init_default();
            result = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &slot->decoder);
            if (result != MA_SUCCESS) {
                logWarn("Audio: Failed to init decoder for '%s' (error %d)\n", sound->name, result);
                return -1;
            }
            slot->ownsDecoder = true;

            result = ma_sound_init_from_data_source(&ma->engine, &slot->decoder, 0, &ma->listenerGroups[0], &slot->maSound);
            if (result != MA_SUCCESS) {
                logWarn("Audio: Failed to init sound from decoder for '%s' (error %d)\n", sound->name, result);
                ma_decoder_uninit(&slot->decoder);
                return -1;
            }
        } else {
            // External audio: load from file
            char* path = resolveExternalPath(ma, sound);
            if (path == nullptr) {
                logWarn("Audio: Could not resolve path for sound '%s'\n", sound->name);
                return -1;
            }

            result = ma_sound_init_from_file(&ma->engine, path, MA_SOUND_FLAG_ASYNC, &ma->listenerGroups[0], nullptr, &slot->maSound);
            if (result != MA_SUCCESS) {
                logWarn("Audio: Failed to load file for '%s' at '%s' (error %d)\n", sound->name, path, result);
                free(path);
                return -1;
            }
            free(path);
            slot->ownsDecoder = false;
        }
    }

    // Apply properties
    float volume = isStream ? streamGain : sound->volume;
    float pitch = isStream ? streamPitch : sound->pitch;
    ma_sound_set_volume(&slot->maSound, volume);
    if (pitch != 1.0f) {
        ma_sound_set_pitch(&slot->maSound, pitch);
    }
    ma_sound_set_looping(&slot->maSound, loop);

    // Set up instance tracking
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->instanceId = SOUND_INSTANCE_ID_BASE + ma->nextInstanceCounter++;
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

static void maSuspend(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    ma_device_stop(ma_engine_get_device(&ma->engine));
}

static void maResume(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    ma_device_start(ma_engine_get_device(&ma->engine));
}

static void maSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - AUDIO_STREAM_INDEX_BASE;

        AudioStreamEntry* stream = &ma->streams[streamSlot];

        if (stream != nullptr) {
            stream->initialGain = gain;
        }

        // We want it to "fallthrough" to the check below so that any playing instances are updated
    }

    if (isValidSoundInstanceId(soundOrInstance)) {
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
        // Before GameMaker 2024.11+, you could NOT change the audio of a streamed OGG file because it went through a path that did NOT support
        // setting the gain of the audio
        //
        // Here's a fun fact for you: https://x.com/MrPowerGamerBR/status/2066291262970356037
        //
        // Thanks YoYo!!!
        if (AUDIO_STREAM_INDEX_BASE > soundOrInstance || DataWin_isVersionAtLeast(audio->dw, 2024, 11, 0, 0)) {
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
}

static float maGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - AUDIO_STREAM_INDEX_BASE;

        AudioStreamEntry* stream = &ma->streams[streamSlot];

        if (stream != nullptr) {
            return stream->initialGain;
        }

        // We want it to "fallthrough" to the check below so that any playing instances are retrieved
    }

    if (isValidSoundInstanceId(soundOrInstance)) {
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

    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - AUDIO_STREAM_INDEX_BASE;

        AudioStreamEntry* stream = &ma->streams[streamSlot];

        if (stream != nullptr) {
            stream->initialPitch = pitch;
        }

        // We want it to "fallthrough" to the check below so that any playing instances are updated
    }

    if (isValidSoundInstanceId(soundOrInstance)) {
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

    if (soundOrInstance >= AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundOrInstance - AUDIO_STREAM_INDEX_BASE;

        AudioStreamEntry* stream = &ma->streams[streamSlot];

        if (stream != nullptr) {
            return stream->initialPitch;
        }

        // We want it to "fallthrough" to the check below so that any playing instances are retrieved
    }

    if (isValidSoundInstanceId(soundOrInstance)) {
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

// Total length of a loaded sound. Works on both SOND index and active instance ids.
// Uses miniaudio's ma_sound_get_length_in_seconds, which reads the decoded duration from the underlying data source (works for fully-decoded sounds AND streaming sounds).
static float maGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    SoundInstance* match = nullptr;
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        match = findInstanceById(ma, soundOrInstance);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                match = inst;
                break;
            }
        }
    }
    if (match != nullptr) {
        float seconds = 0.0f;
        if (ma_sound_get_length_in_seconds(&match->maSound, &seconds) != MA_SUCCESS) return 0.0f;
        return seconds;
    }

    // No active instance: GMS audio_sound_length(soundIndex) must still return the asset's duration.
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE || soundOrInstance >= AUDIO_STREAM_INDEX_BASE)
        return 0.0f;

    DataWin* dw = ma->base.audioGroups[0];
    if (dw == nullptr || 0 > soundOrInstance || (uint32_t) soundOrInstance >= dw->sond.count)
        return 0.0f;

    Sound* sound = &dw->sond.sounds[soundOrInstance];

    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;
    ma_decoder decoder;
    ma_result decResult;
    if (inAudo) {
        if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) return 0.0f;
        DataWin* audioGroup = ma->base.audioGroups[sound->audioGroup];
        DataWin_loadAudoIfNeeded(audioGroup, (uint32_t)sound->audioFile);
        AudioEntry* entry = &audioGroup->audo.entries[sound->audioFile];
        ma_decoder_config decoderConfig = ma_decoder_config_init_default();
        decResult = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &decoder);
    } else {
        char* path = resolveExternalPath(ma, sound);
        if (path == nullptr) return 0.0f;
        ma_decoder_config decoderConfig = ma_decoder_config_init_default();
        decResult = ma_decoder_init_file(path, &decoderConfig, &decoder);
        free(path);
    }
    if (decResult != MA_SUCCESS) return 0.0f;

    ma_uint64 frames = 0;
    float seconds = 0.0f;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS) {
        ma_uint32 sampleRate = decoder.outputSampleRate;
        if (sampleRate > 0) seconds = (float) frames / (float) sampleRate;
    }
    ma_decoder_uninit(&decoder);
    return seconds;
}

static void maSetMasterGain(AudioSystem* audio, float gain) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    ma_engine_set_volume(&ma->engine, gain);
}

static void maSetMasterGainForListener(AudioSystem* audio, float gain, int32_t id) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    if (id < 0 || id >= MAX_LISTENERS) return;
    ma->listenerGains[id] = gain;
    ma_sound_group_set_volume(&ma->listenerGroups[id], gain);
}

static void maSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {
    // miniaudio handles channel management internally, this is a no-op
}

static void maGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex > 0 && audio->dw->agrp.count > (uint32_t) groupIndex) {
        AudioGroup* audioGroupEntry = &audio->dw->agrp.audioGroups[groupIndex];

        char* buf;
        if (audioGroupEntry->path == nullptr) {
            int sz = snprintf(nullptr, 0, "audiogroup%d.dat", groupIndex);
            buf = (char *)safeMalloc(sz + 1);
            snprintf(buf, sz + 1, "audiogroup%d.dat", groupIndex);
        } else {
            size_t length = strlen(audioGroupEntry->path);
            buf = (char *)safeMalloc(length + 1);
            memcpy(buf, audioGroupEntry->path, length);
            buf[length] = '\0';
        }

        // The original runner does not care if the file doesn't exist (this may happen if someone uses "audio_group_load" on a non-existent group)
        FileSystem* fileSystem = ((MaAudioSystem*)audio)->fileSystem;
        if (!fileSystem->vtable->fileExists(fileSystem, buf)) {
            logWarn("Audio: Wanted to load Audio Group %d, but Audio Group %d does not exist in the file system!\n", groupIndex, groupIndex);
            free(buf);
            DataWin* dw = (DataWin *)safeCalloc(1, sizeof(DataWin));
            arrput(audio->audioGroups, dw);
            return;
        }

        DataWinParserOptions options = {0};
        options.parseAudo = true;
        options.lazyLoadAudio = audio->dw->lazyLoadAudio;
        if (audio->dw->mappedFile)
            options.loadType = DATAWINLOADTYPE_MAP_FILE;
        DataWin *audioGroup = DataWin_parse(((MaAudioSystem*)audio)->fileSystem->vtable->resolvePath(((MaAudioSystem*)audio)->fileSystem, buf), options);
        arrput(audio->audioGroups, audioGroup);
        free(buf);
    } else {
        logWarn("Audio: Wanted to load Audio Group %d, but Audio Group %d does not exist in the AGPR!\n", groupIndex, groupIndex);
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
        logWarn("Audio: No free stream slots for '%s'\n", filename);
        return -1;
    }

    char* resolved = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    if (resolved == nullptr) {
        logWarn("Audio: Could not resolve path for stream '%s'\n", filename);
        return -1;
    }

    ma->streams[freeSlot].active = true;
    ma->streams[freeSlot].filePath = resolved;
    ma->streams[freeSlot].initialGain = 1.0f;
    ma->streams[freeSlot].initialPitch = 1.0f;

    int32_t streamIndex = AUDIO_STREAM_INDEX_BASE + freeSlot;
    logInfo("Audio: Created stream %d for '%s' -> '%s'\n", streamIndex, filename, resolved);
    return streamIndex;
}

static bool maDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    int32_t slotIndex = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (0 > slotIndex || slotIndex >= MAX_AUDIO_STREAMS) {
        logWarn("Audio: Invalid stream index %d for destroy\n", streamIndex);
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
    logInfo("Audio: Destroyed stream %d\n", streamIndex);
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable maAudioSystemVtable;

// ===[ Lifecycle ]===

MaAudioSystem* MaAudioSystem_create(DataWin* dataWin) {
    MaAudioSystem* ma = (MaAudioSystem *)safeCalloc(1, sizeof(MaAudioSystem));
    ma->base.dw = dataWin;
    maAudioSystemVtable.init = maInit;
    maAudioSystemVtable.destroy = maDestroy;
    maAudioSystemVtable.update = maUpdate;
    maAudioSystemVtable.playSound = maPlaySound;
    maAudioSystemVtable.stopSound = maStopSound;
    maAudioSystemVtable.stopAll = maStopAll;
    maAudioSystemVtable.isPlaying = maIsPlaying;
    maAudioSystemVtable.pauseSound = maPauseSound;
    maAudioSystemVtable.resumeSound = maResumeSound;
    maAudioSystemVtable.pauseAll = maPauseAll;
    maAudioSystemVtable.resumeAll = maResumeAll;
    maAudioSystemVtable.suspend = maSuspend;
    maAudioSystemVtable.resume = maResume;
    maAudioSystemVtable.setSoundGain = maSetSoundGain;
    maAudioSystemVtable.getSoundGain = maGetSoundGain;
    maAudioSystemVtable.setSoundPitch = maSetSoundPitch;
    maAudioSystemVtable.getSoundPitch = maGetSoundPitch;
    maAudioSystemVtable.getTrackPosition = maGetTrackPosition;
    maAudioSystemVtable.setTrackPosition = maSetTrackPosition;
    maAudioSystemVtable.getSoundLength = maGetSoundLength;
    maAudioSystemVtable.setMasterGain = maSetMasterGain;
    maAudioSystemVtable.setMasterGainForListener = maSetMasterGainForListener;
    maAudioSystemVtable.setChannelCount = maSetChannelCount;
    maAudioSystemVtable.groupLoad = maGroupLoad;
    maAudioSystemVtable.groupIsLoaded = maGroupIsLoaded;
    maAudioSystemVtable.createStream = maCreateStream;
    maAudioSystemVtable.destroyStream = maDestroyStream;
    ma->base.vtable = &maAudioSystemVtable;
    return ma;
}
