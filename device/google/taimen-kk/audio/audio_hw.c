/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_taimen"
/*#define LOG_NDEBUG 0*/

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>

struct pcm_config pcm_config = {
    .channels = 2,
    .rate = 48000,
    .period_size = 960,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    int card;
    int device;
    bool standby;
};

struct stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock; /* see note below on mutex acquisition order */
    struct pcm *pcm;
    bool standby;

    struct audio_device *dev;
};

/**
 * NOTE: when multiple mutexes have to be acquired, always respect the
 * following order: hw device > out stream
 */

/* Helper functions */

/* Pixel 2 XL: MultiMedia1 (PCM 0) -> QUAT_MI2S_RX -> TAS2557 amplifiers (earpiece + speaker) */
static void set_ctl(struct mixer *m, const char *name, const char *str, int val)
{
    struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, name);
    if (!ctl) {
        ALOGW("control not found: %s", name);
        return;
    }
    if (str)
        mixer_ctl_set_enum_by_string(ctl, str);
    else
        mixer_ctl_set_value(ctl, 0, val);
}

static int s_routed;

/* the sound card only exists once the ADSP has booted: retried on every stream open */
static void setup_route(void)
{
    if (s_routed)
        return;
    struct mixer *m = mixer_open(0);
    if (!m) {
        ALOGW("mixer_open(0) failed (sound card not ready yet)");
        return;
    }
    s_routed = 1;
    set_ctl(m, "QUAT_MI2S_RX Channels", "Two", 0);
    set_ctl(m, "QUAT_MI2S_RX Format", "S24_LE", 0);
    set_ctl(m, "Stereo Configuration", NULL, 0);
    set_ctl(m, "Stereo LDAC Playback Volume", NULL, 15);
    set_ctl(m, "Stereo RDAC Playback Volume", NULL, 15);
    set_ctl(m, "QUAT_MI2S_RX Audio Mixer MultiMedia1", NULL, 1);
    mixer_close(m);
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct stream_out *out)
{
    struct audio_device *adev = out->dev;
    int i;

    if ((adev->card < 0) || (adev->device < 0))
        return -EINVAL;

    setup_route();
    out->pcm = pcm_open(adev->card, adev->device, PCM_OUT, &pcm_config);

    if (out->pcm && !pcm_is_ready(out->pcm)) {
        ALOGE("pcm_open() failed: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }

    return 0;
}

/* API functions */

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return pcm_config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    return pcm_config.period_size *
           audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);

    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        out->standby = true;
    }

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return 0;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_out *out = (struct stream_out *)stream;
    struct audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int ret;
    int routing = 0;

    parms = str_parms_create_str(kvpairs);
    pthread_mutex_lock(&adev->lock);

    ret = str_parms_get_str(parms, "card", value, sizeof(value));
    if (ret >= 0)
        adev->card = atoi(value);

    ret = str_parms_get_str(parms, "device", value, sizeof(value));
    if (ret >= 0)
        adev->device = atoi(value);

    pthread_mutex_unlock(&adev->lock);
    str_parms_destroy(parms);

    return 0;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    return (pcm_config.period_size * pcm_config.period_count * 1000) /
            out_get_sample_rate(&stream->common);
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    int ret;
    struct stream_out *out = (struct stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            goto err;
        }
        out->standby = false;
    }

    pcm_write(out->pcm, (void *)buffer, bytes);

    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);

    return bytes;

err:
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    if (ret != 0) {
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
               out_get_sample_rate(&stream->common));
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct audio_device *adev = (struct audio_device *)dev;
    struct stream_out *out;
    int ret;

    out = (struct stream_out *)calloc(1, sizeof(struct stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    out->dev = adev;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    out->standby = true;

    adev->card = 0;
    adev->device = 0;

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct stream_out *out = (struct stream_out *)stream;

    out_standby(&stream->common);
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    return 0;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    return 0;
}


/* ---------------------------------------------------------------------------- capture */
/* Pixel 2 XL: digital microphones of the WCD9340 codec (stock mixer_paths_tavil_taimen.xml):
 *   main (bottom)     : DMIC1 -> DEC7 -> SLIM TX7 -> SLIM_0_TX -> MultiMedia1 (PCM 0 capture)
 *   secondary (top)   : DMIC2 -> DEC5 -> SLIM TX5, for the camcorder
 * 48 kHz, mono, 16-bit; AudioFlinger resamples when needed. */
static struct pcm_config pcm_config_in = {
    .channels = 1,
    .rate = 48000,
    .period_size = 960,
    .period_count = 4,
    .format = PCM_FORMAT_S16_LE,
};

struct stream_in {
    struct audio_stream_in stream;
    pthread_mutex_t lock;
    struct pcm *pcm;
    bool standby;
    int source;
    struct audio_device *dev;
};

static bool s_mic_mute;

static void route_mic(int source)
{
    struct mixer *m = mixer_open(0);
    if (!m)
        return;
    bool secondary = source == AUDIO_SOURCE_CAMCORDER;
    set_ctl(m, "AIF1_CAP Mixer SLIM TX7", NULL, !secondary);
    set_ctl(m, "AIF1_CAP Mixer SLIM TX5", NULL, secondary);
    set_ctl(m, "SLIM_0_TX Channels", "One", 0);
    if (secondary) {
        set_ctl(m, "CDC_IF TX5 MUX", "DEC5", 0);
        set_ctl(m, "ADC MUX5", "DMIC", 0);
        set_ctl(m, "DMIC MUX5", "DMIC2", 0);
        set_ctl(m, "DEC5 Volume", NULL, 88);
    } else {
        set_ctl(m, "CDC_IF TX7 MUX", "DEC7", 0);
        set_ctl(m, "ADC MUX7", "DMIC", 0);
        set_ctl(m, "DMIC MUX7", "DMIC1", 0);
        set_ctl(m, "DEC7 Volume", NULL, 88);
    }
    set_ctl(m, "MultiMedia1 Mixer SLIM_0_TX", NULL, 1);
    mixer_close(m);
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream) { return pcm_config_in.rate; }
static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate) { return -ENOSYS; }
static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    return pcm_config_in.period_size * audio_stream_frame_size(stream);
}
static uint32_t in_get_channels(const struct audio_stream *stream) { return AUDIO_CHANNEL_IN_MONO; }
static audio_format_t in_get_format(const struct audio_stream *stream) { return AUDIO_FORMAT_PCM_16_BIT; }
static int in_set_format(struct audio_stream *stream, audio_format_t format) { return -ENOSYS; }

static int in_standby(struct audio_stream *stream)
{
    struct stream_in *in = (struct stream_in *)stream;
    pthread_mutex_lock(&in->lock);
    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        in->standby = true;
    }
    pthread_mutex_unlock(&in->lock);
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd) { return 0; }

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct stream_in *in = (struct stream_in *)stream;
    struct str_parms *parms = str_parms_create_str(kvpairs);
    char value[32];
    if (str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value)) >= 0)
        in->source = atoi(value);
    str_parms_destroy(parms);
    return 0;
}

static char *in_get_parameters(const struct audio_stream *stream, const char *keys) { return strdup(""); }
static int in_set_gain(struct audio_stream_in *stream, float gain) { return 0; }

static ssize_t in_read(struct audio_stream_in *stream, void *buffer, size_t bytes)
{
    struct stream_in *in = (struct stream_in *)stream;
    int ret = 0;
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        route_mic(in->source);
        in->pcm = pcm_open(0, 0, PCM_IN, &pcm_config_in);
        if (!in->pcm || !pcm_is_ready(in->pcm)) {
            ALOGE("pcm_open(capture): %s", in->pcm ? pcm_get_error(in->pcm) : "?");
            if (in->pcm)
                pcm_close(in->pcm);
            in->pcm = NULL;
            ret = -ENODEV;
        } else {
            in->standby = false;
        }
    }
    if (!ret)
        ret = pcm_read(in->pcm, buffer, bytes);
    pthread_mutex_unlock(&in->lock);
    if (ret == 0 && s_mic_mute)
        memset(buffer, 0, bytes);
    if (ret < 0) {
        /* silence rather than an error that would make AudioFlinger loop */
        memset(buffer, 0, bytes);
        usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) / in_get_sample_rate(&stream->common));
    }
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream) { return 0; }
static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect) { return 0; }
static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect) { return 0; }

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    s_mic_mute = state;
    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    *state = s_mic_mute;
    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    return pcm_config_in.period_size * sizeof(int16_t) *
           popcount(config->channel_mask);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    /* single format: AudioFlinger reopens with the proposed config */
    if (config->sample_rate != pcm_config_in.rate || config->format != AUDIO_FORMAT_PCM_16_BIT ||
        config->channel_mask != AUDIO_CHANNEL_IN_MONO) {
        config->sample_rate = pcm_config_in.rate;
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
        return -EINVAL;
    }
    struct stream_in *in = calloc(1, sizeof(*in));
    if (!in)
        return -ENOMEM;
    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;
    pthread_mutex_init(&in->lock, NULL);
    in->standby = true;
    in->source = AUDIO_SOURCE_MIC;
    in->dev = (struct audio_device *)dev;
    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    in_standby(&stream->common);
    free(stream);
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct audio_device *adev = (struct audio_device *)device;

    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    *device = &adev->hw_device.common;

    setup_route();

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Pixel 2 XL audio HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
