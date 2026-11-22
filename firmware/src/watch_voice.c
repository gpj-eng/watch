#include "watch_board.h"
#include "audio_server.h"
#include <math.h>
#include <rtdevice.h>
#include <rtthread.h>

enum {
    PCM_HZ = 16000,
    PCM_FRAME_MS = 10,
    PCM_FRAME_SAMPLES = PCM_HZ * PCM_FRAME_MS / 1000,
    PCM_CACHE_FRAMES = 4,
    PCM_DMA_TAIL_MS = 2 * PCM_FRAME_MS,
    PCM_WRITE_TIMEOUT_MS = 1000,
    TONE_FIRST_FRAMES = 20,
    TONE_TOTAL_FRAMES = 40,
    TONE_PAUSE_MS = 600,
    SOS_PULSE_FRAMES = 12,
    SOS_PULSE_STRIDE = 20,
    SOS_SIREN_FRAMES = 2 * SOS_PULSE_STRIDE + SOS_PULSE_FRAMES,
    SOS_VOICE_START = SOS_SIREN_FRAMES + 300 / PCM_FRAME_MS,
    SOS_TAIL_FRAMES = 1500 / PCM_FRAME_MS,
};

#define AUDIO_REQUEST_CHANGED (1u << 0)
#define AUDIO_CACHE_AVAILABLE (1u << 1)

static const int16_t voice_help_pcm[] = {
#include "voice_help_pcm.inc"
};
_Static_assert(sizeof(voice_help_pcm) % (PCM_FRAME_SAMPLES * sizeof(int16_t)) == 0,
               "Voice PCM must contain whole 10 ms frames");

struct voice_request {
    enum watch_voice_mode mode;
    uint32_t generation;
};

enum clip_result { CLIP_DONE, CLIP_INTERRUPTED, CLIP_FAILED };

static const alarm_cfg_t *voice_cfg;
static struct rt_event audio_wake;
static struct voice_request requested;
static enum watch_alarm_event completed;
static bool worker_ready;

static struct voice_request current_request(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    struct voice_request value = requested;
    rt_hw_interrupt_enable(level);
    return value;
}

static void wait_audio(rt_uint32_t flags, rt_int32_t ticks)
{
    rt_uint32_t events;
    rt_event_recv(&audio_wake, flags, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, ticks, &events);
}

static bool wait_while_current(uint32_t generation, uint32_t duration_ms)
{
    uint32_t start = watch_now_ms();
    while (current_request().generation == generation) {
        uint32_t elapsed = watch_now_ms() - start;
        if (elapsed >= duration_ms)
            return true;
        wait_audio(AUDIO_REQUEST_CHANGED, rt_tick_from_millisecond(duration_ms - elapsed));
    }
    return false;
}

static int audio_callback(audio_server_callback_cmt_t cmd, void *user_data, uint32_t reserved)
{
    (void)user_data;
    (void)reserved;
    if (cmd == as_callback_cmd_cache_half_empty || cmd == as_callback_cmd_cache_empty)
        rt_event_send(&audio_wake, AUDIO_CACHE_AVAILABLE);
    return 0;
}

static void fill_tone(int16_t *pcm, unsigned frame)
{
    float hz = frame < TONE_FIRST_FRAMES ? 800.0f : 1000.0f;
    for (unsigned i = 0; i < PCM_FRAME_SAMPLES; ++i) {
        unsigned sample = frame * PCM_FRAME_SAMPLES + i;
        pcm[i] = (int16_t)(5500.0f * sinf(6.2831853f * hz * sample / PCM_HZ));
    }
}

static void fill_sos(int16_t *pcm, unsigned frame)
{
    const size_t voice_frames = sizeof(voice_help_pcm) / sizeof(voice_help_pcm[0]) / PCM_FRAME_SAMPLES;
    if (frame >= SOS_VOICE_START && frame < SOS_VOICE_START + voice_frames) {
        const int16_t *source = voice_help_pcm + (frame - SOS_VOICE_START) * PCM_FRAME_SAMPLES;
        for (unsigned i = 0; i < PCM_FRAME_SAMPLES; ++i) {
            int32_t sample = (int32_t)source[i] * 2;
            int32_t magnitude = sample < 0 ? -sample : sample;
            const int32_t knee = 24000;
            if (magnitude > knee) {
                int32_t excess = magnitude - knee;
                magnitude = knee + 6500 * excess / (excess + 7000);
            }
            pcm[i] = (int16_t)(sample < 0 ? -magnitude : magnitude);
        }
    } else if (frame < SOS_SIREN_FRAMES && frame % SOS_PULSE_STRIDE < SOS_PULSE_FRAMES) {
        static const float pitches[] = {2800.0f, 3100.0f, 3400.0f};
        const float phase_step = 6.2831853f * pitches[frame / SOS_PULSE_STRIDE] / PCM_HZ;
        const unsigned fade_samples = PCM_HZ * 5u / 1000u;
        unsigned offset = (frame % SOS_PULSE_STRIDE) * PCM_FRAME_SAMPLES;
        for (unsigned i = 0; i < PCM_FRAME_SAMPLES; ++i) {
            unsigned position = offset + i;
            unsigned remaining = SOS_PULSE_FRAMES * PCM_FRAME_SAMPLES - 1u - position;
            unsigned edge = position < remaining ? position : remaining;
            float envelope = edge < fade_samples ? (float)edge / fade_samples : 1.0f;
            pcm[i] = (int16_t)(26000.0f * envelope * sinf(phase_step * position));
        }
    } else {
        for (unsigned i = 0; i < PCM_FRAME_SAMPLES; ++i)
            pcm[i] = 0;
    }
}

static enum clip_result play_clip(struct voice_request request)
{
    const bool help = request.mode == WATCH_VOICE_HELP;
    const unsigned sos_frames = SOS_VOICE_START + sizeof(voice_help_pcm) /
                                sizeof(voice_help_pcm[0]) / PCM_FRAME_SAMPLES + SOS_TAIL_FRAMES;
    audio_parameter_t params = {
        .write_samplerate = PCM_HZ,
        .write_cache_size = PCM_CACHE_FRAMES * PCM_FRAME_SAMPLES * sizeof(int16_t),
        .write_channnel_num = 1,
        .write_bits_per_sample = 16,
    };
    audio_client_t client = audio_open2(AUDIO_TYPE_ALARM, AUDIO_TX, &params, audio_callback,
                                        NULL, AUDIO_DEVICE_SPEAKER);
    if (!client) {
        rt_pin_write(AW8155_GPIO_PIN, PIN_LOW);
        rt_kprintf("WATCH_AUDIO_ERROR ms=%u open pa42=%d\n", (unsigned)watch_now_ms(),
                   rt_pin_read(AW8155_GPIO_PIN));
        return CLIP_FAILED;
    }

    enum clip_result result = CLIP_INTERRUPTED;
    int16_t tone[PCM_FRAME_SAMPLES];
    unsigned frame = 0;
    uint32_t progressed = watch_now_ms();
    uint8_t old_volume = 0;
    bool volume_changed = false;
    bool amplifier_ready = true;
    if (help && current_request().generation == request.generation) {
        old_volume = audio_server_get_private_volume(AUDIO_TYPE_ALARM);
        unsigned percent = voice_cfg->sos_audio_volume > 100u ? 100u : voice_cfg->sos_audio_volume;
        uint8_t volume = (audio_server_get_max_volume() * percent + 50u) / 100u;
        volume_changed = audio_server_set_private_volume(AUDIO_TYPE_ALARM, volume) == 0;
        amplifier_ready = volume_changed && rt_pin_read(AW8155_GPIO_PIN) == PIN_HIGH;
        rt_kprintf("WATCH_SOS_AUDIO ms=%u volume=%u pa42=%d ready=%d\n", (unsigned)progressed,
                   volume, rt_pin_read(AW8155_GPIO_PIN), amplifier_ready);
    }

    while (amplifier_ready && (help || frame < TONE_TOTAL_FRAMES) &&
            current_request().generation == request.generation) {
        if (help)
            fill_sos(tone, frame);
        else
            fill_tone(tone, frame);
        int written = audio_write(client, (uint8_t *)tone, sizeof(tone));
        if (written == (int)sizeof(tone)) {
            if (help && (frame == 0 || frame == SOS_VOICE_START))
                rt_kprintf("WATCH_SOS_AUDIO ms=%u clip=%s\n", (unsigned)watch_now_ms(),
                           frame == 0 ? "siren" : "help");
            ++frame;
            if (help && frame == sos_frames)
                frame = 0;
            progressed = watch_now_ms();
            continue;
        }
        uint32_t elapsed = watch_now_ms() - progressed;
        if (written != 0 || elapsed >= PCM_WRITE_TIMEOUT_MS) {
            result = CLIP_FAILED;
            break;
        }
        uint32_t wait_ms = PCM_WRITE_TIMEOUT_MS - elapsed;
        wait_audio(AUDIO_REQUEST_CHANGED | AUDIO_CACHE_AVAILABLE, rt_tick_from_millisecond(wait_ms));
    }
    if (!amplifier_ready)
        result = CLIP_FAILED;
    if (!help && frame == TONE_TOTAL_FRAMES) {
        uint32_t queued_ms;
        if (audio_ioctl(client, AUDIO_IOCTL_FLUSH_TIME_MS, &queued_ms) != 0)
            result = CLIP_FAILED;
        else if (wait_while_current(request.generation, queued_ms + PCM_DMA_TAIL_MS))
            result = CLIP_DONE;
    }

    /* The speaker is exclusive to this worker. Mute immediately on abort,
     * then let the SDK shut down the PA and DAC in its normal order. */
    if (help || result != CLIP_DONE)
        rt_pin_write(AW8155_GPIO_PIN, PIN_LOW);
    int close_result = audio_close(client);
    if (close_result || rt_pin_read(AW8155_GPIO_PIN) == PIN_HIGH) {
        rt_pin_write(AW8155_GPIO_PIN, PIN_LOW);
        result = CLIP_FAILED;
    }
    if (volume_changed)
        audio_server_set_private_volume(AUDIO_TYPE_ALARM, old_volume);
    if (help || result == CLIP_FAILED)
        rt_kprintf("WATCH_VOICE_END ms=%u result=%s pa42=%d\n", (unsigned)watch_now_ms(),
                   result == CLIP_DONE ? "done" : result == CLIP_INTERRUPTED ? "stop" : "error",
                   rt_pin_read(AW8155_GPIO_PIN));
    return result;
}

static void publish(uint32_t generation, enum watch_alarm_event event)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (requested.generation == generation)
        completed = event;
    rt_hw_interrupt_enable(level);
}

static void voice_thread(void *unused)
{
    (void)unused;
    uint32_t handled = 0;
    while (true) {
        struct voice_request next = current_request();
        if (handled == next.generation) {
            wait_audio(AUDIO_REQUEST_CHANGED, RT_WAITING_FOREVER);
            continue;
        }
        handled = next.generation;
        if (next.mode == WATCH_VOICE_HELP) {
            while (current_request().generation == handled) {
                enum clip_result result = play_clip(next);
                if (result != CLIP_FAILED) {
                    if (result == CLIP_DONE)
                        publish(handled, EVT_SOS_VOICE_DONE);
                    break;
                }
                publish(handled, EVT_SOS_VOICE_FAILED);
                if (!wait_while_current(handled, 1000u))
                    break;
            }
        } else if (next.mode == WATCH_VOICE_TONE) {
            while (current_request().generation == handled) {
                if (play_clip(next) == CLIP_INTERRUPTED ||
                    !wait_while_current(handled, TONE_PAUSE_MS))
                    break;
            }
        }
    }
}

void watch_voice_init(const alarm_cfg_t *cfg)
{
    voice_cfg = cfg;
    if (rt_event_init(&audio_wake, "voice", RT_IPC_FLAG_PRIO) != RT_EOK)
        return;
    rt_thread_t worker = rt_thread_create("alarmspk", voice_thread, NULL, 3072, 22, 10);
    worker_ready = worker && rt_thread_startup(worker) == RT_EOK;
}

void watch_voice_set(enum watch_voice_mode mode)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool mute_sos = requested.mode == WATCH_VOICE_HELP && mode == WATCH_VOICE_OFF;
    bool changed = requested.mode != mode;
    if (changed) {
        requested.mode = mode;
        ++requested.generation;
        completed = !worker_ready && mode == WATCH_VOICE_HELP ? EVT_SOS_VOICE_FAILED : EVT_ALARM_NONE;
    }
    rt_hw_interrupt_enable(level);
    if (mute_sos) {
        rt_pin_write(AW8155_GPIO_PIN, PIN_LOW);
        rt_kprintf("WATCH_SOS_AUDIO ms=%u muted pa42=%d\n", (unsigned)watch_now_ms(),
                   rt_pin_read(AW8155_GPIO_PIN));
    }
    if (changed && worker_ready)
        rt_event_send(&audio_wake, AUDIO_REQUEST_CHANGED);
}

enum watch_alarm_event watch_voice_take_event(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    enum watch_alarm_event event = completed;
    completed = EVT_ALARM_NONE;
    rt_hw_interrupt_enable(level);
    return event;
}
