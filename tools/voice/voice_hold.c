/* SPDX-License-Identifier: Apache-2.0
 * voice_hold: opens CS-Voice playback+capture PCMs (8 kHz mono S16_LE) and holds them until SIGTERM/SIGINT.
 * In the mainline q6voice driver the DSP voice session starts when both substreams are opened
 * (q6voice_dai_startup -> q6voice_start) and stops when both are closed. No audio data is exchanged.
 * Raw ALSA ioctls: no tinyalsa dependency.   usage: voice_hold [card] [device]   (default 0 4) */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static void any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1; i++)
        memset(&p->masks[i], 0xff, sizeof(p->masks[i]));
    for (int i = 0; i < SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1; i++) {
        p->intervals[i].min = 0;
        p->intervals[i].max = UINT_MAX;
    }
    p->rmask = ~0U;
    p->info = ~0U;
}
static void set_mask(struct snd_pcm_hw_params *p, int param, unsigned val)
{
    struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[val >> 5] = 1U << (val & 31);
}
static void set_int(struct snd_pcm_hw_params *p, int param, unsigned val)
{
    struct snd_interval *i = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = val;
    i->integer = 1;
}

static int open_stream(int card, int dev, char dir)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%d%c", card, dev, dir);
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "voice_hold: open %s: %s\n", path, strerror(errno)); return -1; }

    struct snd_pcm_hw_params hw;
    any(&hw);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    set_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 1);
    set_int(&hw, SNDRV_PCM_HW_PARAM_RATE, 8000);
    /* the kernel picks the remaining parameters (period/buffer) itself */
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        fprintf(stderr, "voice_hold: %s HW_PARAMS: %s\n", path, strerror(errno));
        close(fd); return -1;
    }
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = 1;
    sw.start_threshold = 1;
    sw.stop_threshold = ULONG_MAX;      /* never stop on xrun: no data flows through this PCM */
    sw.silence_size = 0;
    sw.boundary = 0;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        fprintf(stderr, "voice_hold: %s SW_PARAMS: %s\n", path, strerror(errno));
        close(fd); return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        fprintf(stderr, "voice_hold: %s PREPARE: %s\n", path, strerror(errno));
        close(fd); return -1;
    }
    fprintf(stderr, "voice_hold: %s prepared\n", path);
    return fd;
}

int main(int argc, char **argv)
{
    int card = argc > 1 ? atoi(argv[1]) : 0, dev = argc > 2 ? atoi(argv[2]) : 4;
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig);
    int pb = open_stream(card, dev, 'p');
    if (pb < 0) return 1;
    int cp = open_stream(card, dev, 'c');
    if (cp < 0) { close(pb); return 1; }
    /* both substreams are open now (the DSP session starts in q6voice_dai_startup); start them so the AFE ports run */
    int rc = 0;
    if (ioctl(cp, SNDRV_PCM_IOCTL_START) < 0) { fprintf(stderr, "voice_hold: capture START: %s\n", strerror(errno)); rc = 2; }
    if (ioctl(pb, SNDRV_PCM_IOCTL_START) < 0) { fprintf(stderr, "voice_hold: playback START: %s\n", strerror(errno)); rc = 2; }
    fprintf(stderr, "voice_hold: holding (rc=%d)\n", rc);
    while (!stop) pause();
    close(cp); close(pb);
    fprintf(stderr, "voice_hold: closed\n");
    return rc;
}
