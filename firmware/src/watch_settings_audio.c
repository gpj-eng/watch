#include "watch_settings_audio.h"
#include <audio_server.h>
#include <rtthread.h>

#define SAMPLE_RATE 16000u
#define SAMPLE_MS 100u
#define SAMPLE_COUNT (SAMPLE_RATE * SAMPLE_MS / 1000u)

static volatile bool sample_running;
static uint8_t sample_volume;
static int16_t pcm[SAMPLE_COUNT];

static void sample_thread(void *argument)
{
    (void)argument;
    static const int16_t cycle[] = {
        0, 1800, 3410, 4690, 5520, 5800, 5520, 4690, 3410, 1800,
        0, -1800, -3410, -4690, -5520, -5800, -5520, -4690, -3410, -1800,
    };
    for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
        unsigned edge = i < SAMPLE_COUNT - 1 - i ? i : SAMPLE_COUNT - 1 - i;
        int16_t tone = cycle[i % (sizeof(cycle) / sizeof(cycle[0]))];
        pcm[i] = edge < 80u ? (int16_t)(tone * (int)edge / 80) : tone;
    }

    audio_parameter_t parameters = {
        .write_samplerate = SAMPLE_RATE,
        .write_cache_size = 2u * sizeof(pcm),
        .write_channnel_num = 1,
        .write_bits_per_sample = 16,
    };
    uint8_t volume = (audio_server_get_max_volume() * sample_volume + 50u) / 100u;
    audio_client_t client = RT_NULL;
    if (audio_server_set_private_volume(AUDIO_TYPE_NOTIFY, volume) == 0)
        client = audio_open2(AUDIO_TYPE_NOTIFY, AUDIO_TX, &parameters, RT_NULL, RT_NULL, AUDIO_DEVICE_SPEAKER);
    int written = 0;
    if (client) {
        written = audio_write(client, (uint8_t *)pcm, sizeof(pcm));
        if (written > 0)
            rt_thread_mdelay(SAMPLE_MS + 80u);
        audio_close(client);
    }
    rt_kprintf("WATCH_UI sample bytes=%d volume=%u\n", written, (unsigned)volume);
    sample_running = false;
}

bool watch_settings_play_sample(uint8_t percent)
{
    if (sample_running)
        return false;
    if (!percent)
        return true;
    rt_thread_t thread = rt_thread_create("uisound", sample_thread, RT_NULL, 3072, RT_THREAD_PRIORITY_MAX / 2, 10);
    if (!thread)
        return false;
    sample_volume = percent;
    sample_running = true;
    if (rt_thread_startup(thread) != RT_EOK) {
        sample_running = false;
        rt_thread_delete(thread);
        return false;
    }
    return true;
}
