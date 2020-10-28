/*  audio_device.c
 *
 *
 *  Copyright (C) 2014 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic.
 *
 *  Toxic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Toxic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Toxic.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "audio_device.h"

#ifdef AUDIO
#include "audio_call.h"
#endif

#include "line_info.h"
#include "settings.h"
#include "misc_tools.h"

#include <AL/al.h>
#include <AL/alc.h>
/* compatibility with older versions of OpenAL */
#ifndef ALC_ALL_DEVICES_SPECIFIER
#include <AL/alext.h>
#endif /* ALC_ALL_DEVICES_SPECIFIER */

#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

#define inline__ inline __attribute__((always_inline))

extern struct user_settings *user_settings;

typedef struct Device {
    ALCdevice  *dhndl;                     /* Handle of device selected/opened */
    ALCcontext *ctx;                       /* Device context */
    DataHandleCallback cb;                 /* Use this to handle data from input device usually */
    void *cb_data;                         /* Data to be passed to callback */
    int32_t friend_number;                      /* ToxAV friend number */

    uint32_t source, buffers[OPENAL_BUFS]; /* Playback source/buffers */
    uint32_t ref_count;
    int32_t selection;
    bool enable_VAD;
    bool muted;
    pthread_mutex_t mutex[1];
    uint32_t sample_rate;
    uint32_t frame_duration;
    int32_t sound_mode;
#ifdef AUDIO
    float VAD_treshold;                    /* 40 is usually recommended value */
#endif
} Device;

const char *ddevice_names[2];              /* Default device */
const char *devices_names[2][MAX_DEVICES]; /* Container of available devices */
static int size[2];                        /* Size of above containers */
Device *running[2][MAX_DEVICES] = {{NULL}};     /* Running devices */
uint32_t primary_device[2];          /* Primary device */

#ifdef AUDIO
static ToxAV *av = NULL;
#endif /* AUDIO */

/* q_mutex */
#define lock pthread_mutex_lock(&mutex)
#define unlock pthread_mutex_unlock(&mutex)
pthread_mutex_t mutex;


bool thread_running = true,
     thread_paused = true;               /* Thread control */

void *thread_poll(void *);
/* Meet devices */
#ifdef AUDIO
DeviceError init_devices(ToxAV *av_)
#else
DeviceError init_devices(void)
#endif /* AUDIO */
{
    get_devices_names();

    // Start poll thread
    if (pthread_mutex_init(&mutex, NULL) != 0) {
        return de_InternalError;
    }

    pthread_t thread_id;

    if (pthread_create(&thread_id, NULL, thread_poll, NULL) != 0 || pthread_detach(thread_id) != 0) {
        return de_InternalError;
    }

#ifdef AUDIO
    av = av_;
#endif /* AUDIO */

    return (DeviceError) de_None;
}

DeviceError terminate_devices(void)
{
    /* Cleanup if needed */
    lock;
    thread_running = false;
    unlock;

    usleep(20000);

    if (pthread_mutex_destroy(&mutex) != 0) {
        return (DeviceError) de_InternalError;
    }

    return (DeviceError) de_None;
}

void get_devices_names(void)
{

    const char *stringed_device_list;

    size[input] = 0;

    if ((stringed_device_list = alcGetString(NULL, ALC_CAPTURE_DEVICE_SPECIFIER))) {
        ddevice_names[input] = alcGetString(NULL, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);

        for (; *stringed_device_list && size[input] < MAX_DEVICES; ++size[input]) {
            devices_names[input][size[input]] = stringed_device_list;
            stringed_device_list += strlen(stringed_device_list) + 1;
        }
    }

    size[output] = 0;

    if (alcIsExtensionPresent(NULL, "ALC_ENUMERATE_ALL_EXT") != AL_FALSE) {
        stringed_device_list = alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
    } else {
        stringed_device_list = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
    }

    if (stringed_device_list) {
        ddevice_names[output] = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);

        for (; *stringed_device_list && size[output] < MAX_DEVICES; ++size[output]) {
            devices_names[output][size[output]] = stringed_device_list;
            stringed_device_list += strlen(stringed_device_list) + 1;
        }
    }
}

DeviceError device_mute(DeviceType type, uint32_t device_idx)
{
    if (device_idx >= MAX_DEVICES) {
        return de_InvalidSelection;
    }

    lock;

    Device *device = running[type][device_idx];

    if (!device) {
        unlock;
        return de_DeviceNotActive;
    }

    device->muted = !device->muted;

    unlock;
    return de_None;
}

#ifdef AUDIO
DeviceError device_set_VAD_treshold(uint32_t device_idx, float value)
{
    if (device_idx >= MAX_DEVICES) {
        return de_InvalidSelection;
    }

    lock;

    Device *device = running[input][device_idx];

    if (!device) {
        unlock;
        return de_DeviceNotActive;
    }

    device->VAD_treshold = value;

    unlock;
    return de_None;
}
#endif


DeviceError set_primary_device(DeviceType type, int32_t selection)
{
    if (size[type] <= selection || selection < 0) {
        return de_InvalidSelection;
    }

    primary_device[type] = selection;

    return de_None;
}

DeviceError open_primary_device(DeviceType type, uint32_t *device_idx, uint32_t sample_rate, uint32_t frame_duration,
                                uint8_t channels)
{
    return open_device(type, primary_device[type], device_idx, sample_rate, frame_duration, channels);
}

void get_primary_device_name(DeviceType type, char *buf, int size)
{
    memcpy(buf, ddevice_names[type], size);
}

// TODO: generate buffers separately
DeviceError open_device(DeviceType type, int32_t selection, uint32_t *device_idx, uint32_t sample_rate,
                        uint32_t frame_duration, uint8_t channels)
{
    if (size[type] <= selection || selection < 0) {
        return de_InvalidSelection;
    }

    if (channels != 1 && channels != 2) {
        return de_UnsupportedMode;
    }

    lock;

    const uint32_t frame_size = (sample_rate * frame_duration / 1000);

    uint32_t i;

    for (i = 0; i < MAX_DEVICES && running[type][i] != NULL; ++i);

    if (i == MAX_DEVICES) {
        unlock;
        return de_AllDevicesBusy;
    } else {
        *device_idx = i;
    }

    for (i = 0; i < MAX_DEVICES; i ++) { /* Check if any device has the same selection */
        if (running[type][i] && running[type][i]->selection == selection) {
//             printf("a%d-%d:%p ", selection, i, running[type][i]->dhndl);

            running[type][*device_idx] = running[type][i];
            running[type][i]->ref_count ++;

            unlock;
            return de_None;
        }
    }

    Device *device = running[type][*device_idx] = calloc(1, sizeof(Device));
    device->selection = selection;

    device->sample_rate = sample_rate;
    device->frame_duration = frame_duration;
    device->sound_mode = channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

    if (pthread_mutex_init(device->mutex, NULL) != 0) {
        free(device);
        unlock;
        return de_InternalError;
    }

    if (type == input) {
        device->dhndl = alcCaptureOpenDevice(devices_names[type][selection],
                                             sample_rate, device->sound_mode, frame_size * 2);
#ifdef AUDIO
        device->VAD_treshold = user_settings->VAD_treshold;
#endif
    } else {
        device->dhndl = alcOpenDevice(devices_names[type][selection]);

        if (!device->dhndl) {
            free(device);
            running[type][*device_idx] = NULL;
            unlock;
            return de_FailedStart;
        }

        size_t zeros_size = frame_size * sizeof(uint16_t);
        uint16_t *zeros = calloc(1, zeros_size);

        if (zeros == NULL) {
            free(device);
            running[type][*device_idx] = NULL;
            unlock;
            return de_FailedStart;
        }

        device->ctx = alcCreateContext(device->dhndl, NULL);
        alcMakeContextCurrent(device->ctx);

        alGenBuffers(OPENAL_BUFS, device->buffers);
        alGenSources((uint32_t)1, &device->source);
        alSourcei(device->source, AL_LOOPING, AL_FALSE);

        for (i = 0; i < OPENAL_BUFS; ++i) {
            alBufferData(device->buffers[i], device->sound_mode, zeros, zeros_size, sample_rate);
        }

        free(zeros);

        alSourceQueueBuffers(device->source, OPENAL_BUFS, device->buffers);
        alSourcePlay(device->source);
    }

    if (alcGetError(device->dhndl) != AL_NO_ERROR) {
        free(device);
        running[type][*device_idx] = NULL;
        unlock;
        return de_FailedStart;
    }

    if (type == input) {
        alcCaptureStart(device->dhndl);
        thread_paused = false;
    }

    unlock;
    return de_None;
}

DeviceError close_device(DeviceType type, uint32_t device_idx)
{
    if (device_idx >= MAX_DEVICES) {
        return de_InvalidSelection;
    }

    lock;

    Device *device = running[type][device_idx];
    DeviceError rc = de_None;

    if (!device) {
        unlock;
        return de_DeviceNotActive;
    }

    running[type][device_idx] = NULL;

    if (!device->ref_count) {
        if (type == input) {
            if (!alcCaptureCloseDevice(device->dhndl)) {
                rc = de_AlError;
            }
        } else {
            if (alcGetCurrentContext() != device->ctx) {
                alcMakeContextCurrent(device->ctx);
            }

            alDeleteSources(1, &device->source);
            alDeleteBuffers(OPENAL_BUFS, device->buffers);

            alcMakeContextCurrent(NULL);

            if (device->ctx) {
                alcDestroyContext(device->ctx);
            }

            if (!alcCloseDevice(device->dhndl)) {
                rc = de_AlError;
            }
        }

        free(device);
    } else {
        device->ref_count--;
    }

    unlock;
    return rc;
}

DeviceError register_device_callback(int32_t friend_number, uint32_t device_idx, DataHandleCallback callback,
                                     void *data, bool enable_VAD)
{
    if (size[input] <= device_idx || !running[input][device_idx] || running[input][device_idx]->dhndl == NULL) {
        return de_InvalidSelection;
    }

    lock;
    running[input][device_idx]->cb = callback;
    running[input][device_idx]->cb_data = data;
    running[input][device_idx]->enable_VAD = enable_VAD;
    running[input][device_idx]->friend_number = friend_number;
    unlock;

    return de_None;
}

inline__ DeviceError write_out(uint32_t device_idx, const int16_t *data, uint32_t sample_count, uint8_t channels,
                               uint32_t sample_rate)
{
    if (device_idx >= MAX_DEVICES) {
        return de_InvalidSelection;
    }

    Device *device = running[output][device_idx];

    if (!device || device->muted) {
        return de_DeviceNotActive;
    }

    pthread_mutex_lock(device->mutex);


    ALuint bufid;
    ALint processed, queued;
    alGetSourcei(device->source, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(device->source, AL_BUFFERS_QUEUED, &queued);

    if (processed) {
        ALuint *bufids = malloc(processed * sizeof(ALuint));

        if (bufids == NULL) {
            return de_InternalError;
        }

        alSourceUnqueueBuffers(device->source, processed, bufids);
        alDeleteBuffers(processed - 1, bufids + 1);
        bufid = bufids[0];
        free(bufids);
    } else if (queued < 16) {
        alGenBuffers(1, &bufid);
    } else {
        pthread_mutex_unlock(device->mutex);
        return de_Busy;
    }


    alBufferData(bufid, channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, data, sample_count * 2 * channels,
                 sample_rate);
    alSourceQueueBuffers(device->source, 1, &bufid);

    ALint state;
    alGetSourcei(device->source, AL_SOURCE_STATE, &state);

    if (state != AL_PLAYING) {
        alSourcePlay(device->source);
    }


    pthread_mutex_unlock(device->mutex);
    return de_None;
}

#define FRAME_BUF_SIZE 16000

void *thread_poll(void *arg)  // TODO: maybe use thread for every input source
{
    /*
     * NOTE: We only need to poll input devices for data.
     */
    UNUSED_VAR(arg);
    int32_t sample = 0;

    int16_t *frame_buf = malloc(FRAME_BUF_SIZE * sizeof(int16_t));

    if (frame_buf == NULL) {
        exit_toxic_err("failed in thread_poll", FATALERR_MEMORY);
    }

    while (1) {
        lock;

        if (!thread_running) {
            free(frame_buf);
            unlock;
            break;
        }

        bool paused = thread_paused;
        unlock;

        /* Wait for unpause. */
        if (paused) {
            usleep(10000);
        }

        else {
            for (uint32_t i = 0; i < size[input]; ++i) {
                lock;

                Device *device = running[input][i];

                if (device != NULL) {
                    alcGetIntegerv(device->dhndl, ALC_CAPTURE_SAMPLES, sizeof(int32_t), &sample);

                    int f_size = (device->sample_rate * device->frame_duration / 1000);

                    if (sample < f_size || f_size > FRAME_BUF_SIZE) {
                        unlock;
                        continue;
                    }

                    alcCaptureSamples(device->dhndl, frame_buf, f_size);

                    if (device->muted) {
                        unlock;
                        continue;
                    }

                    if (device->cb) {
                        device->cb(frame_buf, f_size, device->cb_data);
                    }
                }

                unlock;
            }

            usleep(5000);
        }
    }

    pthread_exit(NULL);
}

void print_devices(ToxWindow *self, DeviceType type)
{
    int i;

    for (i = 0; i < size[type]; ++i) {
        line_info_add(self, NULL, NULL, NULL, SYS_MSG, i == primary_device[type] ? 1 : 0, 0, "%d: %s", i,
                      devices_names[type][i]);
    }

    return;
}

DeviceError selection_valid(DeviceType type, int32_t selection)
{
    return (size[type] <= selection || selection < 0) ? de_InvalidSelection : de_None;
}

void *get_device_callback_data(uint32_t device_idx)
{
    if (size[input] <= device_idx || !running[input][device_idx] || running[input][device_idx]->dhndl == NULL) {
        return NULL;
    }

    return running[input][device_idx]->cb_data;
}
