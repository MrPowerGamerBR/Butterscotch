#include "input_recording.h"
#include "json_reader.h"
#include "json_writer.h"

#include "utils.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"

#include "stb_ds.h"

InputRecording* InputRecording_createRecorder(const char* filePath) {
    InputRecording* rec = (InputRecording *)safeCalloc(1, sizeof(InputRecording));
    rec->isRecording = true;
    rec->recordFilePath = filePath;
    return rec;
}

InputRecording* InputRecording_createPlayer(const char* playbackFilePath, const char* recordFilePath) {
    // Read the file contents
    FILE* f = fopen(playbackFilePath, "rb");
    if (f == nullptr) {
        logError("Could not open input recording file '%s'\n", playbackFilePath);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* contents = (char *)safeMalloc(fileSize + 1);
    safeFread(contents, fileSize, f, playbackFilePath);
    contents[fileSize] = '\0';
    fclose(f);

    // Parse JSON
    JsonValue* root = JsonReader_parse(contents);
    free(contents);

    if (root == nullptr || !JsonReader_isObject(root)) {
        logError("Invalid JSON in input recording file '%s'\n", playbackFilePath);
        exit(1);
    }

    // Find the highest frame number to determine array size
    int objectLen = JsonReader_objectLength(root);
    int32_t maxFrame = -1;
    {
    repeat(objectLen, i) {
        const char* key = JsonReader_getJsonKeyByIndex(root, i);
        int32_t frameNum = (int32_t) strtol(key, nullptr, 10);
        if (frameNum > maxFrame) maxFrame = frameNum;
    }
    }

    InputRecording* rec = (InputRecording *)safeCalloc(1, sizeof(InputRecording));
    rec->isPlayback = true;
    rec->playbackFrameCount = maxFrame + 1;

    // If a record path was provided, also enable recording
    if (recordFilePath != nullptr) {
        rec->isRecording = true;
        rec->recordFilePath = recordFilePath;
    }

    // Allocate playbackFrames array (one stb_ds int32_t array per frame)
    rec->playbackFrames = (InputFrame *)safeCalloc(rec->playbackFrameCount, sizeof(InputFrame));

    repeat(objectLen, i) {
        const char* key = JsonReader_getJsonKeyByIndex(root, i);
        JsonValue* val = JsonReader_getJsonValueByIndex(root, i);
        int32_t frameNum = (int32_t) strtol(key, nullptr, 10);

        JsonValue* keysPressed = JsonReader_getJsonValueByKey(val, "keysPressed");
        JsonValue* keysReleased = JsonReader_getJsonValueByKey(val, "keysReleased");

        int32_t keysPressedLength = JsonReader_arrayLength(keysPressed);
        int32_t keysReleasedLength = JsonReader_arrayLength(keysReleased);

        int32_t* keysPressedArray = nullptr;
        int32_t* keysReleasedArray = nullptr;

        {
        repeat(keysPressedLength, j) {
            arrput(keysPressedArray, JsonReader_getInt(JsonReader_getArrayElement(keysPressed, j)));
        }
        }

        repeat(keysReleasedLength, j) {
            arrput(keysReleasedArray, JsonReader_getInt(JsonReader_getArrayElement(keysReleased, j)));
        }

        InputFrame inputFrame = {0};
        inputFrame.keysPressed = keysPressedArray;
        inputFrame.keysReleased = keysReleasedArray;
        rec->playbackFrames[frameNum] = inputFrame;
    }

    JsonReader_free(root);
    logInfo("InputRecording: Loaded %d frames from '%s'\n", rec->playbackFrameCount, playbackFilePath);
    return rec;
}

void InputRecording_free(InputRecording* recording) {
    if (recording == nullptr) return;

    if (recording->recordedFrames != nullptr) {
        repeat(arrlen(recording->recordedFrames), i) {
            InputFrame frame = recording->recordedFrames[i];
            arrfree(frame.keysPressed);
            arrfree(frame.keysReleased);
        }

        arrfree(recording->recordedFrames);
    }

    if (recording->playbackFrames != nullptr) {
        repeat(recording->playbackFrameCount, i) {
            InputFrame frame = recording->playbackFrames[i];
            arrfree(frame.keysPressed);
            arrfree(frame.keysReleased);
        }

        free(recording->playbackFrames);
    }

    free(recording);
}

void InputRecording_processFrame(InputRecording* recording, RunnerKeyboardState* kb, int frameNumber) {
    if (recording == nullptr) return;

    // Playback: send the pressed keys from the recorded data
    if (recording->isPlayback) {
        if (recording->playbackFrameCount > frameNumber) {
            InputFrame frame = recording->playbackFrames[frameNumber];
            int32_t keyPressedCount = (int32_t) arrlen(frame.keysPressed);
            int32_t keyReleasedCount = (int32_t) arrlen(frame.keysReleased);

            repeat(keyPressedCount, i) {
                RunnerKeyboard_onKeyDown(kb, frame.keysPressed[i]);
            }

            {
            repeat(keyReleasedCount, i) {
                RunnerKeyboard_onKeyUp(kb, frame.keysReleased[i]);
            }
            }
        } else {
            if (!recording->playbackEnded) {
                logInfo("InputRecording: Playback ended at frame %d (recorded %d frames)\n", frameNumber, recording->playbackFrameCount);
                recording->playbackEnded = true;
            }
        }
    }

    // Recording: snapshot whatever the current keyboard state is (from real input or playback)
    if (recording->isRecording) {
        int32_t* keysPressed = nullptr;
        int32_t* keysReleased = nullptr;

        repeat(GML_KEY_COUNT, key) {
            if (recording->filterDebugKeys && (key == 'P' || key == 'O')) continue;
            if (kb->keyPressed[key]) {
                arrput(keysPressed, (int32_t) key);
            }
            if (kb->keyReleased[key]) {
                arrput(keysReleased, (int32_t) key);
            }
        }

        InputFrame inputFrame = {0};
        inputFrame.keysPressed = keysPressed;
        inputFrame.keysReleased = keysReleased;

        arrput(recording->recordedFrames, inputFrame);
    }
}

bool InputRecording_save(InputRecording* recording) {
    if (recording == nullptr || !recording->isRecording) return false;

    int32_t frameCount = (int32_t) arrlen(recording->recordedFrames);

    JsonWriter w = JsonWriter_create();
    JsonWriter_beginObject(&w);

    repeat(frameCount, f) {
        // Frame number as string key
        char frameKey[16];
        snprintf(frameKey, sizeof(frameKey), "%d", (int) f);
        JsonWriter_key(&w, frameKey);

        InputFrame frame = recording->recordedFrames[f];
        JsonWriter_beginObject(&w);

        JsonWriter_key(&w, "keysPressed");
        JsonWriter_beginArray(&w);

        repeat(arrlen(frame.keysPressed), i) {
            JsonWriter_int(&w, frame.keysPressed[i]);
        }

        JsonWriter_endArray(&w);

        JsonWriter_key(&w, "keysReleased");
        JsonWriter_beginArray(&w);

        {
        repeat(arrlen(frame.keysReleased), i) {
            JsonWriter_int(&w, frame.keysReleased[i]);
        }
        }

        JsonWriter_endArray(&w);

        JsonWriter_endObject(&w);
    }

    JsonWriter_endObject(&w);

    FILE* file = fopen(recording->recordFilePath, "wb");
    if (file == nullptr) {
        logWarn("Error: Could not write input recording to '%s'\n", recording->recordFilePath);
        JsonWriter_free(&w);
        return false;
    }

    const char* output = JsonWriter_getOutput(&w);
    size_t len = JsonWriter_getLength(&w);
    if (fwrite(output, 1, len, file) != len) {
        logWarn("Error: Could not write input recording to '%s'\n", recording->recordFilePath);
        JsonWriter_free(&w);
        fclose(file);
        return false;
    }
    fputc('\n', file);
    fclose(file);

    logInfo("InputRecording: Saved %d frames to '%s'\n", frameCount, recording->recordFilePath);
    JsonWriter_free(&w);
    return true;
}

bool InputRecording_isPlaybackActive(InputRecording* recording) {
    return recording != nullptr && recording->isPlayback && !recording->playbackEnded;
}
