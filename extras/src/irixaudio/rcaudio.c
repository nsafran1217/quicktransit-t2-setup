/*
 * libRCAudio.so: native (ia64) half of the IRIX libaudio replacement.
 *
 * QuickTransit dlopens this from FU_TARGETNBLIBRARY_PATH when the IRIX
 * libaudio.so declares it (see rcstubs.S), and the IRIX side calls the
 * RC_qta_* functions directly from translated code.  Each IRIX audio port
 * is one ALSA PCM on $QTAUDIO_DEVICE (default "default"), so the program
 * plays like any other Linux ALSA client.
 *
 * Handles are small table indexes: the IRIX side is 32-bit and never sees
 * a host pointer.  Sample data is read in place from IRIX memory, which is
 * identity-mapped and big-endian; integer formats go to ALSA as their _BE
 * variants and the plug layer converts.  Float and double are scaled by
 * floatmax, clipped and converted here to host S32: a plug in front of dmix
 * can't take float at 44.1 kHz (no hw_params fits), but takes S32.
 *
 * ALSA only ever gets whole periods.  A resampling plug converts a period
 * at a time, and the last partial period of a sound would sit in the plug,
 * counted as played, until an underrun throws it away.  So a partial
 * period is held here until more data completes it, or until the program
 * has stopped writing and the device is down to its last period, when it
 * goes out padded with silence.  Padding while the program still streams
 * would put gaps in the middle of the sound.  Held frames don't count as
 * queued: SDL, for one, keeps its queue only half to fully filled, and with
 * a large period (dmix) the held part would be most of that, leaving ALSA
 * nearly dry.
 *
 * QTAUDIO_DEBUG=1 prints the ALSA configuration of each port and every
 * underrun.
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Formats, as passed by the IRIX side (libaudio.c QTA_FMT_*). */
enum { FMT_S8 = 1, FMT_S16 = 2, FMT_S24 = 3, FMT_FLOAT = 4, FMT_DOUBLE = 5 };

#define NPORT	32

static struct port {
    snd_pcm_t *pcm;
    pthread_mutex_t mu;		/* write and flush vs. a polling thread */
    int fmt, channels, rate;
    unsigned inbytes;		/* IRIX-side bytes per frame */
    unsigned outbytes;		/* ALSA-side bytes per frame */
    double scale;		/* full scale/floatmax for float and double */
    snd_pcm_uframes_t bufsize, period;
    unsigned char *hold;	/* one period, ALSA format */
    long held;			/* frames in hold */
    long long last;		/* time of the last write, ns */
    int32_t *conv;		/* one period, float and double ports */
} port[NPORT];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int debug;

static struct port *getport(int h)
{
    if (h < 0 || h >= NPORT || !port[h].pcm)
        return NULL;
    return &port[h];
}

static double fmax_from_bits(unsigned bits)
{
    union { unsigned u; float f; } v;

    v.u = bits;
    return v.f > 0 ? v.f : 1.0f;
}

/*
 * Like snd_pcm_set_params, but every size is "near", so odd combinations
 * (22050 Hz 8-bit mono through a plug) still get a configuration.  The
 * device starts once a period is queued.
 */
static int setup(snd_pcm_t *pcm, snd_pcm_format_t fmt, int channels,
                 unsigned rate, unsigned latency)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t period;
    unsigned bt = latency, pt = latency / 8;
    int err;

    snd_pcm_hw_params_alloca(&hw);
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (err = snd_pcm_hw_params_set_access(pcm, hw,
                                SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_resample(pcm, hw, 1)) < 0 ||
        (err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0)
        return err;
    snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &bt, 0);
    snd_pcm_hw_params_set_period_time_near(pcm, hw, &pt, 0);
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
        return err;

    snd_pcm_hw_params_get_period_size(hw, &period, 0);
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
    return snd_pcm_sw_params(pcm, sw);
}

/*
 * Open a playback PCM.  queue_frames is the IRIX port's queue size; it sets
 * the ALSA buffer time, within limits.  Returns a handle or -errno.
 */
int RC_qta_open(int rate, int channels, int fmt, int queue_frames,
                unsigned floatmax_bits)
{
    static const snd_pcm_format_t alsafmt[] = {
        [FMT_S8] = SND_PCM_FORMAT_S8,
        [FMT_S16] = SND_PCM_FORMAT_S16_BE,
        [FMT_S24] = SND_PCM_FORMAT_S24_BE,	/* 24 bits in 32, LSB-aligned */
        [FMT_FLOAT] = SND_PCM_FORMAT_S32,
        [FMT_DOUBLE] = SND_PCM_FORMAT_S32,
    };
    static const unsigned samplebytes[] = { 0, 1, 2, 4, 4, 8 };
    const char *dev = getenv("QTAUDIO_DEVICE");
    snd_pcm_t *pcm;
    unsigned latency;
    struct port *p;
    int h, err;

    debug = getenv("QTAUDIO_DEBUG") != NULL;
    if (fmt < FMT_S8 || fmt > FMT_DOUBLE || channels < 1 || rate < 1000)
        return -EINVAL;
    if (!dev || !*dev)
        dev = "default";

    /* At least 80 ms: dmix fixes the period (often ~20 ms), and a buffer
       of two periods underruns under QuickTransit. */
    latency = (unsigned)((long long)queue_frames * 1000000 / rate);
    if (latency < 80000)
        latency = 80000;
    if (latency > 1000000)
        latency = 1000000;

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "libRCAudio: cannot open ALSA device '%s': %s\n",
                dev, snd_strerror(err));
        return err;
    }
    err = setup(pcm, alsafmt[fmt], channels, rate, latency);
    if (err < 0) {
        fprintf(stderr, "libRCAudio: '%s' rejects %d Hz, %d ch, %s: %s\n",
                dev, rate, channels, snd_pcm_format_name(alsafmt[fmt]),
                snd_strerror(err));
        snd_pcm_close(pcm);
        return err;
    }

    pthread_mutex_lock(&lock);
    for (h = 0; h < NPORT && port[h].pcm; h++)
        ;
    if (h == NPORT) {
        pthread_mutex_unlock(&lock);
        snd_pcm_close(pcm);
        return -EMFILE;
    }
    p = &port[h];
    memset(p, 0, sizeof *p);
    p->pcm = pcm;
    pthread_mutex_unlock(&lock);

    pthread_mutex_init(&p->mu, NULL);
    p->fmt = fmt;
    p->channels = channels;
    p->rate = rate;
    p->inbytes = samplebytes[fmt] * channels;
    p->outbytes = snd_pcm_frames_to_bytes(pcm, 1);
    p->scale = 2147483647.0 / fmax_from_bits(floatmax_bits);
    snd_pcm_get_params(pcm, &p->bufsize, &p->period);
    p->hold = malloc(p->period * p->outbytes);
    if (fmt == FMT_FLOAT || fmt == FMT_DOUBLE)
        p->conv = malloc(p->period * p->outbytes);
    if (debug)
        fprintf(stderr, "libRCAudio: port %d on '%s': %d Hz, %d ch, %s, "
                "buffer %lu, period %lu\n", h, dev, rate, channels,
                snd_pcm_format_name(alsafmt[fmt]), p->bufsize, p->period);
    return h;
}

static uint32_t be32(const unsigned char *b)
{
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* IRIX float or double samples, +-floatmax -> host S32, clipped. */
static void convert(struct port *p, const unsigned char *src, long n)
{
    union { uint32_t u; float f; } f;
    union { uint64_t u; double d; } d;
    double v;
    long i;

    for (i = 0; i < n; i++) {
        if (p->fmt == FMT_FLOAT) {
            f.u = be32(src);
            v = f.f;
            src += 4;
        } else {
            d.u = (uint64_t)be32(src) << 32 | be32(src + 4);
            v = d.d;
            src += 8;
        }
        v *= p->scale;
        p->conv[i] = v >= 2147483647.0 ? 2147483647 :
                     v <= -2147483648.0 ? INT32_MIN : (int32_t)v;
    }
}

/* Write ALSA-format frames, blocking.  Underruns play on, as on IRIX. */
static int out(struct port *p, const unsigned char *src, long n)
{
    snd_pcm_sframes_t r;

    while (n > 0) {
        r = snd_pcm_writei(p->pcm, src, n);
        if (r < 0) {
            if (debug)
                fprintf(stderr, "libRCAudio: %s\n", snd_strerror(r));
            if (r == -EINTR)
                continue;
            if (r == -ESTRPIPE)
                while (snd_pcm_resume(p->pcm) == -EAGAIN)
                    usleep(10000);
            if ((r = snd_pcm_prepare(p->pcm)) < 0)
                return r;
            continue;
        }
        src += r * p->outbytes;
        n -= r;
    }
    return 0;
}

/* Queue ALSA-format frames: whole periods go out, the rest is held. */
static int push(struct port *p, const unsigned char *src, long n)
{
    long k;
    int err;

    if (p->held) {
        k = (long)p->period - p->held;
        if (k > n)
            k = n;
        memcpy(p->hold + p->held * p->outbytes, src, k * p->outbytes);
        p->held += k;
        src += k * p->outbytes;
        n -= k;
        if (p->held < (long)p->period)
            return 0;
        p->held = 0;
        if ((err = out(p, p->hold, p->period)) < 0)
            return err;
    }
    k = n - n % p->period;
    if (k && (err = out(p, src, k)) < 0)
        return err;
    memcpy(p->hold, src + k * p->outbytes, (n - k) * p->outbytes);
    p->held = n - k;
    return 0;
}

/* Send the held partial period, padded with silence (zero bits). */
static int flush(struct port *p)
{
    if (!p->held)
        return 0;
    memset(p->hold + p->held * p->outbytes, 0,
           (p->period - p->held) * p->outbytes);
    p->held = 0;
    return out(p, p->hold, p->period);
}

/* Frames ALSA has queued and not played; 0 after an underrun. */
static long alsa_filled(struct port *p)
{
    snd_pcm_sframes_t avail = snd_pcm_avail(p->pcm);

    if (avail < 0 || (snd_pcm_uframes_t)avail >= p->bufsize)
        return 0;
    return p->bufsize - avail;
}

static long long now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/*
 * Send the held partial period, padded, and start a port that never
 * reached the start threshold (IRIX plays whatever is written).
 */
static void kick(struct port *p)
{
    flush(p);
    if (snd_pcm_state(p->pcm) == SND_PCM_STATE_PREPARED)
        snd_pcm_start(p->pcm);
}

/*
 * From alGetFilled.  If ALSA has run dry, or no write came for a period's
 * time and ALSA is down to its last period, nothing more is coming in
 * time, so the held frames go out.  Then a program waiting for
 * alGetFilled() == 0 hears them before it sees 0.
 */
static void kick_idle(struct port *p)
{
    long f;

    if (!p->held)
        return;
    f = alsa_filled(p);
    if (f == 0 || (f <= (long)p->period &&
        now() - p->last >= (long long)p->period * 1000000000 / p->rate))
        kick(p);
}

/*
 * Write frames, blocking until all are queued (alWriteFrames semantics).
 * buf == NULL writes silence (alZeroFrames).  Returns frames or -errno.
 */
int RC_qta_write(int h, const unsigned char *buf, int frames)
{
    struct port *p = getport(h);
    long done = 0, n;
    int err = 0, idle;

    if (!p)
        return -EBADF;
    pthread_mutex_lock(&p->mu);
    /* Into an empty device (first write, a one-shot sound, or after an
       underrun) nothing may follow in time: send the tail now.  While the
       program streams, hold it for the next write. */
    idle = alsa_filled(p) == 0;
    while (done < frames && !err) {
        n = frames - done;
        if (n > (long)p->period)
            n = p->period;
        if (!buf) {
            /* zero bits are silence in every ALSA format used here */
            static const unsigned char zero[8192];

            if (n * p->outbytes > sizeof zero)
                n = sizeof zero / p->outbytes;
            err = push(p, zero, n);
        } else if (p->conv) {
            convert(p, buf + done * p->inbytes, n * p->channels);
            err = push(p, (unsigned char *)p->conv, n);
        } else
            err = push(p, buf + done * p->inbytes, n);
        done += n;
    }
    if (!err && idle)
        kick(p);
    p->last = now();
    pthread_mutex_unlock(&p->mu);
    return err < 0 ? err : done;
}

/* Frames queued in ALSA and not yet played (alGetFilled). */
int RC_qta_filled(int h)
{
    struct port *p = getport(h);
    long f;

    if (!p)
        return -EBADF;
    if (pthread_mutex_trylock(&p->mu) != 0)	/* a write is in progress */
        return alsa_filled(p);
    kick_idle(p);
    f = alsa_filled(p);
    pthread_mutex_unlock(&p->mu);
    return f;
}

/* Frames until the next written frame is heard (alGetFrameTime). */
int RC_qta_delay(int h)
{
    struct port *p = getport(h);
    snd_pcm_sframes_t d;

    if (!p)
        return -EBADF;
    if (snd_pcm_delay(p->pcm, &d) < 0 || d < 0)
        d = 0;
    return d + p->held;
}

/*
 * drain != 0: play out what is queued first.  Otherwise discard it, unless
 * nothing but the held partial period is left: a program that waited for
 * alGetFilled() == 0 before closing expects to hear everything.
 */
int RC_qta_close(int h, int drain)
{
    struct port *p = getport(h);

    if (!p)
        return -EBADF;
    pthread_mutex_lock(&p->mu);
    if (drain || alsa_filled(p) == 0) {
        flush(p);
        snd_pcm_drain(p->pcm);
    } else
        snd_pcm_drop(p->pcm);
    pthread_mutex_unlock(&p->mu);
    snd_pcm_close(p->pcm);
    pthread_mutex_destroy(&p->mu);
    free(p->hold);
    free(p->conv);
    pthread_mutex_lock(&lock);
    memset(p, 0, sizeof *p);
    pthread_mutex_unlock(&lock);
    return 0;
}
