/*
 * libaudio.so for IRIX programs under QuickTransit.
 *
 * Replaces SGI's libaudio (the Audio Library, AL), which drives the hdsp
 * kernel driver that QuickTransit doesn't emulate.  This one keeps the AL
 * bookkeeping (configs, ports, parameters, errors) on the IRIX side and
 * hands sample data to libRCAudio.so, a native ALSA library, through
 * QuickTransit's SCall binding (rcstubs.S).  The program then plays through
 * ALSA's default device like any Linux program.
 *
 * Output only: input ports fail to open, event queues never deliver events,
 * connections and most hardware parameters aren't there.  Every public
 * function of SGI's lib32 libaudio.so.1 is exported, so programs link.
 *
 * Build (IRIX gcc, see docs/irix-gcc.md):
 *   gcc -O2 -Wall -shared -fPIC -static-libgcc -Wl,-soname,libaudio.so \
 *       -o libaudio.so libaudio.c rcstubs.S
 */
#include <dmedia/audio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syssgi.h>
#include <sys/time.h>

/* rcstubs.S; formats match rcaudio.c */
enum { QTA_FMT_S8 = 1, QTA_FMT_S16, QTA_FMT_S24, QTA_FMT_FLOAT, QTA_FMT_DOUBLE };
extern void qta_libdecl(void);
extern long long qta_open(int rate, int channels, int fmt, int queue_frames,
                          unsigned floatmax_bits);
extern long long qta_write(int h, const void *buf, int frames);
extern long long qta_filled(int h);
extern long long qta_delay(int h);
extern long long qta_close(int h, int drain);

#define DEFAULT_RATE	44100
#define DEFAULT_QUEUE	50000	/* frames */
#define MAX_PORTS	32

struct _ALconfig {
    int queue;		/* frames */
    int queue_samps;	/* set through ALsetqueuesize (samples), else 0 */
    int width, sampfmt, channels, device, limiting;
    double floatmax;
};

struct _ALport {
    struct _ALconfig c;
    int h;		/* libRCAudio handle, -1 while closed */
    int rate;		/* rate h was opened at */
    int dirty;		/* config changed: reopen before the next write */
    int fillpoint;	/* frames */
    int fd[2];		/* alGetFD pipe */
    int resource;
    unsigned long long frames;	/* frames written */
};

static int out_rate = DEFAULT_RATE;
static int nports;
static ALerrfunc errfunc = AL_PRINT_ERRORS;

static const char *const errstr[] = {
    "not implemented", "bad port", "bad config", "bad device or resource",
    "cannot access device", "bad direction", "out of memory",
    "no ports available", "bad sample width", "illegal state",
    "bad queue size", "bad fillpoint", "null buffer pointer",
    "negative count", "bad param/value buffer", "negative buffer length",
    "odd buffer length", "bad channel count", "bad parameter",
    "bad sample format", "bad rate", "bad transfer size", "bad floatmax",
    "bad port style", "bad buffer length", "not found", "permission denied",
};

static void alerror(int code, const char *fmt, ...)
{
    char msg[200];
    va_list ap;

    setoserror(code);
    if (errfunc == AL_NO_ERRORS)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (errfunc == AL_PRINT_ERRORS)
        fprintf(stderr, "Audio Library: %s\n", msg);
    else
        errfunc(code, "%s", msg);
}

#define UNSUPPORTED() \
    alerror(AL_BAD_NOT_IMPLEMENTED, "%s: not supported under QuickTransit", \
            __func__)

static unsigned long long ust_now(void)
{
    unsigned long long ust = 0;
    struct timeval tv;

    if (syssgi(SGI_GET_UST, &ust, 0) == 0 && ust)
        return ust;
    gettimeofday(&tv, 0);
    return tv.tv_sec * 1000000000ULL + tv.tv_usec * 1000ULL;
}

/* ---- configs ---- */

static int queue_frames(const struct _ALconfig *c)
{
    return c->queue_samps ? c->queue_samps / c->channels : c->queue;
}

static int qta_fmt(const struct _ALconfig *c)
{
    if (c->sampfmt == AL_SAMPFMT_FLOAT)
        return QTA_FMT_FLOAT;
    if (c->sampfmt == AL_SAMPFMT_DOUBLE)
        return QTA_FMT_DOUBLE;
    return c->width == AL_SAMPLE_8 ? QTA_FMT_S8 :
           c->width == AL_SAMPLE_24 ? QTA_FMT_S24 : QTA_FMT_S16;
}

static int sample_bytes(const struct _ALconfig *c)
{
    static const int b[] = { 0, 1, 2, 4, 4, 8 };

    return b[qta_fmt(c)];
}

ALconfig alNewConfig(void)
{
    ALconfig c = calloc(1, sizeof *c);

    if (!c) {
        alerror(AL_BAD_OUT_OF_MEM, "alNewConfig: out of memory");
        return 0;
    }
    c->queue = DEFAULT_QUEUE;
    c->width = AL_SAMPLE_16;
    c->sampfmt = AL_SAMPFMT_TWOSCOMP;
    c->channels = AL_STEREO;
    c->device = AL_DEFAULT_OUTPUT;
    c->floatmax = 1.0;
    return c;
}

#define CHECK_CONFIG(c, fail) \
    if (!(c)) { alerror(AL_BAD_CONFIG, "%s: bad config", __func__); return fail; }
#define CHECK_PORT(p, fail) \
    if (!(p)) { alerror(AL_BAD_PORT, "%s: bad port", __func__); return fail; }

int alFreeConfig(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    free(c);
    return 0;
}

int alSetQueueSize(ALconfig c, int n)
{
    CHECK_CONFIG(c, -1);
    if (n < 1) {
        alerror(AL_BAD_QSIZE, "alSetQueueSize: bad queue size %d", n);
        return -1;
    }
    c->queue = n;
    c->queue_samps = 0;
    return 0;
}

int alGetQueueSize(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    return queue_frames(c);
}

int alSetWidth(ALconfig c, int w)
{
    CHECK_CONFIG(c, -1);
    if (w != AL_SAMPLE_8 && w != AL_SAMPLE_16 && w != AL_SAMPLE_24) {
        alerror(AL_BAD_WIDTH, "alSetWidth: bad width %d", w);
        return -1;
    }
    c->width = w;
    return 0;
}

int alGetWidth(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    return c->width;
}

int alSetSampFmt(ALconfig c, int f)
{
    CHECK_CONFIG(c, -1);
    if (f != AL_SAMPFMT_TWOSCOMP && f != AL_SAMPFMT_FLOAT &&
        f != AL_SAMPFMT_DOUBLE) {
        alerror(AL_BAD_SAMPFMT, "alSetSampFmt: bad sample format %d", f);
        return -1;
    }
    c->sampfmt = f;
    return 0;
}

int alGetSampFmt(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    return c->sampfmt;
}

int alSetChannels(ALconfig c, int n)
{
    CHECK_CONFIG(c, -1);
    if (n < 1 || n > 8) {
        alerror(AL_BAD_CHANNELS, "alSetChannels: bad channel count %d", n);
        return -1;
    }
    c->channels = n;
    return 0;
}

int alGetChannels(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    return c->channels;
}

int alSetFloatMax(ALconfig c, double m)
{
    CHECK_CONFIG(c, -1);
    if (!(m > 0)) {
        alerror(AL_BAD_FLOATMAX, "alSetFloatMax: bad floatmax");
        return -1;
    }
    c->floatmax = m;
    return 0;
}

double alGetFloatMax(ALconfig c)
{
    CHECK_CONFIG(c, 0);
    return c->floatmax;
}

int alSetLimiting(ALconfig c, int l)
{
    CHECK_CONFIG(c, -1);
    c->limiting = l;
    return 0;
}

int alGetLimiting(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    return c->limiting;
}

int alSetDevice(ALconfig c, int d)
{
    CHECK_CONFIG(c, -1);
    c->device = d;
    return 0;
}

int alGetDevice(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    return c->device;
}

/* ---- ports ---- */

static void ensure_declared(void)
{
    static int done;

    /* Translating qta_libdecl makes QuickTransit dlopen libRCAudio.so; it
       has to happen before any other stub is first called. */
    if (!done) {
        qta_libdecl();
        done = 1;
    }
}

static int port_open_native(ALport p)
{
    union { float f; unsigned u; } fm;
    int h;

    ensure_declared();
    fm.f = (float)p->c.floatmax;
    h = (int)qta_open(out_rate, p->c.channels, qta_fmt(&p->c),
                      queue_frames(&p->c), fm.u);
    if (h < 0) {
        alerror(AL_BAD_DEVICE_ACCESS, "cannot open the audio device "
                "(ALSA error %d, see libRCAudio message)", h);
        return -1;
    }
    p->h = h;
    p->rate = out_rate;
    p->dirty = 0;
    return 0;
}

/* Reopen the native port if the rate or the sample format changed. */
static int port_sync(ALport p)
{
    if (p->h >= 0 && (p->dirty || p->rate != out_rate)) {
        qta_close(p->h, 1);
        p->h = -1;
    }
    return p->h >= 0 ? 0 : port_open_native(p);
}

ALport alOpenPort(const char *name, const char *dir, ALconfig c)
{
    static int next_resource = 0x1000;
    ALport p;

    (void)name;
    if (!dir || (dir[0] != 'r' && dir[0] != 'w')) {
        alerror(AL_BAD_DIRECTION, "alOpenPort: bad direction");
        return 0;
    }
    if (dir[0] == 'r') {
        alerror(AL_BAD_DEVICE_ACCESS,
                "alOpenPort: audio input is not supported under QuickTransit");
        return 0;
    }
    if (nports >= MAX_PORTS) {
        alerror(AL_BAD_NO_PORTS, "alOpenPort: no ports available");
        return 0;
    }
    p = calloc(1, sizeof *p);
    if (!p) {
        alerror(AL_BAD_OUT_OF_MEM, "alOpenPort: out of memory");
        return 0;
    }
    if (c)
        p->c = *c;
    else {
        ALconfig d = alNewConfig();

        if (!d) {
            free(p);
            return 0;
        }
        p->c = *d;
        free(d);
    }
    p->h = -1;
    p->fd[0] = p->fd[1] = -1;
    p->fillpoint = queue_frames(&p->c) / 2;
    if (port_open_native(p) < 0) {
        free(p);
        return 0;
    }
    p->resource = next_resource++;
    nports++;
    return p;
}

int alClosePort(ALport p)
{
    CHECK_PORT(p, -1);
    if (p->h >= 0)
        qta_close(p->h, 0);
    if (p->fd[0] >= 0) {
        close(p->fd[0]);
        close(p->fd[1]);
    }
    free(p);
    nports--;
    return 0;
}

int alGetResource(ALport p)
{
    CHECK_PORT(p, 0);
    return p->resource;
}

ALconfig alGetConfig(ALport p)
{
    ALconfig c;

    CHECK_PORT(p, 0);
    c = alNewConfig();
    if (c)
        *c = p->c;
    return c;
}

/* Width, sample format and floatmax can change on an open port. */
int alSetConfig(ALport p, ALconfig c)
{
    CHECK_PORT(p, -1);
    CHECK_CONFIG(c, -1);
    if (c->width != p->c.width || c->sampfmt != p->c.sampfmt ||
        c->floatmax != p->c.floatmax || c->channels != p->c.channels)
        p->dirty = 1;
    p->c.width = c->width;
    p->c.sampfmt = c->sampfmt;
    p->c.floatmax = c->floatmax;
    p->c.channels = c->channels;
    return 0;
}

/* Output ports are always ready as far as select() is concerned: a write
   that doesn't fit just blocks. */
int alGetFD(ALport p)
{
    CHECK_PORT(p, -1);
    if (p->fd[1] < 0 && pipe(p->fd) < 0) {
        alerror(AL_BAD_OUT_OF_MEM, "alGetFD: pipe failed");
        return -1;
    }
    return p->fd[1];
}

int alGetFilled(ALport p)
{
    CHECK_PORT(p, -1);
    return p->h >= 0 ? (int)qta_filled(p->h) : 0;
}

int alGetFillable(ALport p)
{
    int n;

    CHECK_PORT(p, -1);
    n = queue_frames(&p->c) - alGetFilled(p);
    return n > 0 ? n : 0;
}

int alWriteFrames(ALport p, void *buf, int n)
{
    int r;

    CHECK_PORT(p, -1);
    if (n < 0) {
        alerror(AL_BAD_COUNT_NEG, "alWriteFrames: negative count");
        return -1;
    }
    if (!buf) {
        alerror(AL_BAD_BUFFER_NULL, "alWriteFrames: null buffer");
        return -1;
    }
    if (n == 0)
        return 0;
    if (port_sync(p) < 0)
        return -1;
    r = (int)qta_write(p->h, buf, n);
    if (r < 0) {
        alerror(AL_BAD_DEVICE_ACCESS, "alWriteFrames: ALSA error %d", r);
        return -1;
    }
    p->frames += r;
    return 0;
}

int alZeroFrames(ALport p, int n)
{
    int r;

    CHECK_PORT(p, -1);
    if (n < 0) {
        alerror(AL_BAD_COUNT_NEG, "alZeroFrames: negative count");
        return -1;
    }
    if (n == 0)
        return 0;
    if (port_sync(p) < 0)
        return -1;
    r = (int)qta_write(p->h, 0, n);
    if (r < 0) {
        alerror(AL_BAD_DEVICE_ACCESS, "alZeroFrames: ALSA error %d", r);
        return -1;
    }
    p->frames += r;
    return 0;
}

/* Non-interleaved write: interleave a block at a time, then write it. */
int alWriteBuffers(ALport p, void **bufs, int *strides, int n)
{
    char block[4096];
    int ch, sb, fb, per, done, k, i;

    CHECK_PORT(p, -1);
    if (n < 0) {
        alerror(AL_BAD_COUNT_NEG, "alWriteBuffers: negative count");
        return -1;
    }
    if (!bufs) {
        alerror(AL_BAD_BUFFER_NULL, "alWriteBuffers: null buffer");
        return -1;
    }
    ch = p->c.channels;
    sb = sample_bytes(&p->c);
    fb = sb * ch;
    per = sizeof block / fb;
    for (done = 0; done < n; done += k) {
        k = n - done < per ? n - done : per;
        for (i = 0; i < ch; i++) {
            int stride = strides ? strides[i] : 1;
            const char *src = bufs[i];
            int f;

            for (f = 0; f < k; f++)
                if (src)
                    memcpy(block + f * fb + i * sb,
                           src + (long)(done + f) * stride * sb, sb);
                else
                    memset(block + f * fb + i * sb, 0, sb);
        }
        if (alWriteFrames(p, block, k) < 0)
            return -1;
    }
    return 0;
}

int alReadFrames(ALport p, void *buf, int n)
{
    (void)buf; (void)n;
    CHECK_PORT(p, -1);
    alerror(AL_BAD_DIRECTION, "alReadFrames: not an input port");
    return -1;
}

int alReadBuffers(ALport p, void **bufs, int *strides, int n)
{
    (void)bufs; (void)strides; (void)n;
    CHECK_PORT(p, -1);
    alerror(AL_BAD_DIRECTION, "alReadBuffers: not an input port");
    return -1;
}

int alDiscardFrames(ALport p, int n)
{
    (void)n;
    CHECK_PORT(p, -1);
    alerror(AL_BAD_DIRECTION, "alDiscardFrames: not an input port");
    return -1;
}

int alSetFillPoint(ALport p, int n)
{
    CHECK_PORT(p, -1);
    if (n < 0 || n > queue_frames(&p->c)) {
        alerror(AL_BAD_FILLPOINT, "alSetFillPoint: bad fillpoint %d", n);
        return -1;
    }
    p->fillpoint = n;
    return 0;
}

int alGetFillPoint(ALport p)
{
    CHECK_PORT(p, -1);
    return p->fillpoint;
}

int alGetFrameNumber(ALport p, stamp_t *fnum)
{
    CHECK_PORT(p, -1);
    if (fnum)
        *fnum = p->frames;
    return 0;
}

/* The next frame written will reach the jack after the queued ones. */
int alGetFrameTime(ALport p, stamp_t *fnum, stamp_t *time)
{
    long long d;

    CHECK_PORT(p, -1);
    d = p->h >= 0 ? qta_delay(p->h) : 0;
    if (fnum)
        *fnum = p->frames;
    if (time)
        *time = ust_now() + d * 1000000000LL / p->rate;
    return 0;
}

/* ---- parameters and resources ---- */

double alFixedToDouble(ALfixed x)
{
    return x / 4294967296.0;
}

ALfixed alDoubleToFixed(double d)
{
    return (ALfixed)(d * 4294967296.0);
}

static void put_string(ALpv *pv, const char *s)
{
    int n = strlen(s) + 1;

    if (pv->value.ptr && pv->sizeIn > 0) {
        if (n > pv->sizeIn)
            n = pv->sizeIn;
        memcpy(pv->value.ptr, s, n);
        ((char *)pv->value.ptr)[n - 1] = 0;
        pv->sizeOut = n;
    }
}

int alGetParams(int res, ALpv *pv, int n)
{
    int i, j, good = 0;
    int input = res == AL_DEFAULT_INPUT;

    if (!pv || n < 0) {
        alerror(AL_BAD_PVBUFFER, "alGetParams: bad param/value buffer");
        return -1;
    }
    for (i = 0; i < n; i++) {
        pv[i].sizeOut = 1;
        switch (pv[i].param) {
        case AL_RATE:
            pv[i].value.ll = alIntToFixed(out_rate);
            break;
        case AL_MASTER_CLOCK:
            pv[i].value.i = AL_CRYSTAL_MCLK_TYPE;
            break;
        case AL_RATE_FRACTION_N:
        case AL_RATE_FRACTION_D:
            pv[i].value.i = 1;
            break;
        case AL_GAIN:		/* vector of per-channel dB: 0 dB */
            pv[i].sizeOut = 0;
            if (pv[i].value.ptr)
                for (j = 0; j < pv[i].sizeIn && j < 2; j++) {
                    ((ALfixed *)pv[i].value.ptr)[j] = 0;
                    pv[i].sizeOut = j + 1;
                }
            break;
        case AL_MUTE:
            pv[i].value.i = 0;
            break;
        case AL_CHANNELS:
            pv[i].value.i = 2;
            break;
        case AL_TYPE:
            pv[i].value.i = input ? AL_INPUT_DEVICE_TYPE : AL_OUTPUT_DEVICE_TYPE;
            break;
        case AL_NAME:
        case AL_LABEL:
            put_string(&pv[i], input ? "AnalogIn" : "AnalogOut");
            break;
        case AL_PORT_COUNT:
            pv[i].value.i = nports;
            break;
        case AL_MAX_PORTS:
            pv[i].value.i = MAX_PORTS;
            break;
        case AL_UNUSED_PORTS:
            pv[i].value.i = MAX_PORTS - nports;
            break;
        default:
            pv[i].sizeOut = -1;
            continue;
        }
        good++;
    }
    return good;
}

/* Only the rate means anything; gain, mute and clock are accepted. */
int alSetParams(int res, ALpv *pv, int n)
{
    int i, good = 0;

    (void)res;
    if (!pv || n < 0) {
        alerror(AL_BAD_PVBUFFER, "alSetParams: bad param/value buffer");
        return -1;
    }
    for (i = 0; i < n; i++) {
        pv[i].sizeOut = 0;
        switch (pv[i].param) {
        case AL_RATE: {
            /* fixed point; value.i (the high word) also works for callers
               who set it as an int */
            long long r = (pv[i].value.ll + 0x80000000LL) >> 32;

            if (r >= 1000 && r <= 192000)
                out_rate = (int)r;
            break;
        }
        case AL_MASTER_CLOCK:
        case AL_RATE_FRACTION_N:
        case AL_RATE_FRACTION_D:
        case AL_GAIN:
        case AL_MUTE:
            break;
        default:
            pv[i].sizeOut = -1;
            continue;
        }
        good++;
    }
    return good;
}

int alGetParamInfo(int res, int param, ALparamInfo *pi)
{
    if (!pi) {
        alerror(AL_BAD_BUFFER_NULL, "alGetParamInfo: null buffer");
        return -1;
    }
    memset(pi, 0, sizeof *pi);
    pi->resource = res;
    pi->param = param;
    pi->maxElems = pi->maxElems2 = 1;
    pi->operations = AL_GET_OP | AL_SET_OP;
    switch (param) {
    case AL_RATE:
        pi->valueType = AL_SCALAR_VAL;
        pi->elementType = AL_FIXED_ELEM;
        strcpy(pi->name, "Rate");
        pi->initial.ll = alIntToFixed(DEFAULT_RATE);
        pi->min.ll = alIntToFixed(4000);
        pi->max.ll = alIntToFixed(192000);
        pi->minDelta.ll = alIntToFixed(1);
        pi->maxDelta.ll = alIntToFixed(188000);
        break;
    case AL_GAIN:
        pi->valueType = AL_VECTOR_VAL;
        pi->elementType = AL_FIXED_ELEM;
        pi->maxElems = 2;
        strcpy(pi->name, "Gain");
        pi->min.ll = alIntToFixed(-96);
        pi->max.ll = 0;
        pi->minDelta.ll = alIntToFixed(1);
        pi->maxDelta.ll = alIntToFixed(96);
        break;
    case AL_MUTE:
        pi->valueType = AL_SCALAR_VAL;
        pi->elementType = AL_INT32_ELEM;
        strcpy(pi->name, "Mute");
        pi->max.i = 1;
        break;
    default:
        alerror(AL_BAD_PARAM, "alGetParamInfo: unsupported parameter %#x",
                param);
        return -1;
    }
    return 0;
}

int alQueryValues(int res, int param, ALvalue *set, int setsize,
                  ALpv *quals, int qualsize)
{
    (void)res; (void)param; (void)set; (void)setsize; (void)quals;
    (void)qualsize;
    UNSUPPORTED();
    return -1;
}

/* There is one output device; anything that sounds like input is input. */
int alGetResourceByName(int res, char *name, int type)
{
    (void)res;
    if (type == AL_INPUT_DEVICE_TYPE || type == AL_INPUT_PORT_TYPE ||
        (name && (strstr(name, "In") || strstr(name, "Mic"))))
        return AL_DEFAULT_INPUT;
    return AL_DEFAULT_OUTPUT;
}

int alIsSubtype(int type, int sub)
{
    if (type == sub || type == AL_RESOURCE_TYPE)
        return 1;
    if (type == AL_DEVICE_TYPE)
        return sub == AL_INPUT_DEVICE_TYPE || sub == AL_OUTPUT_DEVICE_TYPE;
    if (type == AL_PORT_TYPE)
        return sub == AL_INPUT_PORT_TYPE || sub == AL_OUTPUT_PORT_TYPE;
    return 0;
}

int alConnect(int src, int dst, ALpv *pv, int n)
{
    (void)src; (void)dst; (void)pv; (void)n;
    UNSUPPORTED();
    return -1;
}

int alDisconnect(int c)
{
    (void)c;
    UNSUPPORTED();
    return -1;
}

ALerrfunc alSetErrorHandler(ALerrfunc f)
{
    ALerrfunc old = errfunc;

    errfunc = f;
    return old;
}

char *alGetErrorString(int e)
{
    static char buf[40];

    if (e >= 0 && e < (int)(sizeof errstr / sizeof errstr[0]))
        return (char *)errstr[e];
    switch (e) {
    case AL_BAD_NO_EVENTQS:	return "no event queues available";
    case AL_BAD_INVALID_EVENTQ:	return "invalid event queue";
    case AL_BAD_INVALID_EVENT:	return "invalid event";
    case AL_BAD_EVENT_NOT_FOUND: return "event not found";
    }
    sprintf(buf, "unknown audio error %d", e);
    return buf;
}

/* ---- events: a queue that never receives anything ---- */

struct _ALeventQueue { int fd[2]; };
struct _ALevent { int dummy; };

ALeventQueue alOpenEventQueue(const char *name)
{
    ALeventQueue q = malloc(sizeof *q);

    (void)name;
    if (!q || pipe(q->fd) < 0) {
        free(q);
        alerror(AL_BAD_NO_EVENTQS, "alOpenEventQueue: no event queues");
        return 0;
    }
    return q;
}

int alCloseEventQueue(ALeventQueue q)
{
    if (!q)
        return -1;
    close(q->fd[0]);
    close(q->fd[1]);
    free(q);
    return 0;
}

ALevent alNewEvent(void) { return calloc(1, sizeof(struct _ALevent)); }
int alFreeEvent(ALevent e) { free(e); return 0; }
int alSelectEvents(ALeventQueue q, int r, int *p, int n)
{ (void)q; (void)r; (void)p; (void)n; return 0; }
int alDeselectEvents(ALeventQueue q, int r, int *p, int n)
{ (void)q; (void)r; (void)p; (void)n; return 0; }
int alPendingEvents(ALeventQueue q) { (void)q; return 0; }
int alFlushEvents(ALeventQueue q) { (void)q; return 0; }
int alGetEventQueueFD(ALeventQueue q) { return q ? q->fd[0] : -1; }

/* Blocks like IRIX's would when nothing ever happens. */
int alNextEvent(ALeventQueue q, ALevent e)
{
    char c;

    (void)e;
    if (!q)
        return -1;
    while (read(q->fd[0], &c, 1) < 0 && errno == EINTR)
        ;
    return -1;
}

int alCheckEvent(ALeventQueue q, int r, int p, ALevent e)
{
    (void)q; (void)r; (void)p; (void)e;
    setoserror(AL_BAD_EVENT_NOT_FOUND);
    return -1;
}

int alGetEventParam(ALevent e) { (void)e; return 0; }
int alGetEventResource(ALevent e) { (void)e; return 0; }
int alGetEventSrcResource(ALevent e) { (void)e; return 0; }
stamp_t alGetEventTime(ALevent e) { (void)e; return 0; }
long long alGetEventUST(ALevent e) { (void)e; return 0; }
void *alGetEventData(ALevent e) { (void)e; return 0; }

ALvalue alGetEventValue(ALevent e)
{
    ALvalue v;

    (void)e;
    v.ll = 0;
    return v;
}

/* ---- old AL (AL1): counts are samples, not frames ---- */

ALconfig ALnewconfig(void) { return alNewConfig(); }
int ALfreeconfig(ALconfig c) { return alFreeConfig(c); }
int ALsetconfig(ALport p, ALconfig c) { return alSetConfig(p, c); }
ALconfig ALgetconfig(ALport p) { return alGetConfig(p); }
int ALsetwidth(ALconfig c, long w) { return alSetWidth(c, w); }
long ALgetwidth(ALconfig c) { return alGetWidth(c); }
int ALsetsampfmt(ALconfig c, long f) { return alSetSampFmt(c, f); }
long ALgetsampfmt(ALconfig c) { return alGetSampFmt(c); }
int ALsetchannels(ALconfig c, long n) { return alSetChannels(c, n); }
long ALgetchannels(ALconfig c) { return alGetChannels(c); }
int ALsetfloatmax(ALconfig c, double m) { return alSetFloatMax(c, m); }
double ALgetfloatmax(ALconfig c) { return alGetFloatMax(c); }
ALport ALopenport(const char *name, const char *dir, ALconfig c)
{ return alOpenPort(name, dir, c); }
int ALcloseport(ALport p) { return alClosePort(p); }
int ALgetfd(ALport p) { return alGetFD(p); }
ALerrfunc ALseterrorhandler(ALerrfunc f) { return alSetErrorHandler(f); }

int ALsetqueuesize(ALconfig c, long n)
{
    CHECK_CONFIG(c, -1);
    if (n < 1) {
        alerror(AL_BAD_QSIZE, "ALsetqueuesize: bad queue size %ld", n);
        return -1;
    }
    c->queue_samps = n;
    return 0;
}

long ALgetqueuesize(ALconfig c)
{
    CHECK_CONFIG(c, -1);
    return c->queue_samps ? c->queue_samps : c->queue * c->channels;
}

long ALgetfilled(ALport p)
{
    CHECK_PORT(p, -1);
    return (long)alGetFilled(p) * p->c.channels;
}

long ALgetfillable(ALport p)
{
    CHECK_PORT(p, -1);
    return (long)alGetFillable(p) * p->c.channels;
}

int ALwritesamps(ALport p, void *buf, long n)
{
    CHECK_PORT(p, -1);
    return alWriteFrames(p, buf, n / p->c.channels);
}

int ALreadsamps(ALport p, void *buf, long n)
{
    return alReadFrames(p, buf, n);
}

int ALsetfillpoint(ALport p, long n)
{
    CHECK_PORT(p, -1);
    return alSetFillPoint(p, n / p->c.channels);
}

long ALgetfillpoint(ALport p)
{
    CHECK_PORT(p, -1);
    return (long)p->fillpoint * p->c.channels;
}

int ALgetframenumber(ALport p, unsigned long long *fnum)
{
    return alGetFrameNumber(p, (stamp_t *)fnum);
}

int ALgetframetime(ALport p, unsigned long long *fnum,
                   unsigned long long *time)
{
    return alGetFrameTime(p, (stamp_t *)fnum, (stamp_t *)time);
}

static long al1_value(long param)
{
    switch (param) {
    case AL_INPUT_RATE:
    case AL_OUTPUT_RATE:
        return out_rate;
    case AL_LEFT_SPEAKER_GAIN:
    case AL_RIGHT_SPEAKER_GAIN:
        return 255;
    case AL_INPUT_SOURCE:
        return AL_INPUT_LINE;
    case AL_OUTPUT_COUNT:
        return nports;
    case AL_UNUSED_COUNT:
        return MAX_PORTS - nports;
    case AL_CHANNEL_MODE:
        return AL_STEREO;
    case AL_DIGITAL_INPUT_RATE:
        return AL_RATE_NO_DIGITAL_INPUT;
    default:		/* attenuations, counts, monitor and mute: 0 / off */
        return 0;
    }
}

int ALgetparams(long dev, long *pv, long n)
{
    long i;

    (void)dev;
    if (!pv || n < 0 || (n & 1)) {
        alerror(AL_BAD_PVBUFFER, "ALgetparams: bad param/value buffer");
        return -1;
    }
    for (i = 0; i < n; i += 2)
        pv[i + 1] = al1_value(pv[i]);
    return 0;
}

int ALsetparams(long dev, long *pv, long n)
{
    long i;

    (void)dev;
    if (!pv || n < 0 || (n & 1)) {
        alerror(AL_BAD_PVBUFFER, "ALsetparams: bad param/value buffer");
        return -1;
    }
    for (i = 0; i < n; i += 2)
        if (pv[i] == AL_OUTPUT_RATE && pv[i + 1] >= 1000 &&
            pv[i + 1] <= 192000)
            out_rate = pv[i + 1];
    return 0;
}

long ALgetdefault(long dev, long param)
{
    (void)dev;
    if (param == AL_INPUT_RATE || param == AL_OUTPUT_RATE)
        return DEFAULT_RATE;
    return al1_value(param);
}

int ALgetminmax(long dev, long param, long *min, long *max)
{
    long lo = 0, hi = 0;

    (void)dev;
    if (param == AL_INPUT_RATE || param == AL_OUTPUT_RATE) {
        lo = AL_RATE_8000;
        hi = AL_RATE_48000;
    } else if (param != AL_INPUT_SOURCE && param != AL_CHANNEL_MODE)
        hi = 255;
    if (min)
        *min = lo;
    if (max)
        *max = hi;
    return 0;
}

static const struct { long param, type; const char *name; } al1params[] = {
    { AL_INPUT_SOURCE, AL_ENUM_VALUE, "Input Source" },
    { AL_LEFT_INPUT_ATTEN, AL_RANGE_VALUE, "Left Input Atten" },
    { AL_RIGHT_INPUT_ATTEN, AL_RANGE_VALUE, "Right Input Atten" },
    { AL_INPUT_RATE, AL_ENUM_VALUE, "Input Rate" },
    { AL_OUTPUT_RATE, AL_ENUM_VALUE, "Output Rate" },
    { AL_LEFT_SPEAKER_GAIN, AL_RANGE_VALUE, "Left Speaker Gain" },
    { AL_RIGHT_SPEAKER_GAIN, AL_RANGE_VALUE, "Right Speaker Gain" },
    { AL_INPUT_COUNT, AL_RANGE_VALUE, "Input Count" },
    { AL_OUTPUT_COUNT, AL_RANGE_VALUE, "Output Count" },
    { AL_UNUSED_COUNT, AL_RANGE_VALUE, "Unused Count" },
    { AL_MONITOR_CTL, AL_ENUM_VALUE, "Monitor Control" },
    { AL_SPEAKER_MUTE_CTL, AL_ENUM_VALUE, "Speaker Mute Control" },
    { AL_CHANNEL_MODE, AL_ENUM_VALUE, "Channel Mode" },
};
#define NAL1PARAMS (long)(sizeof al1params / sizeof al1params[0])

long ALqueryparams(long dev, long *pv, long max)
{
    long i;

    (void)dev;
    for (i = 0; i < NAL1PARAMS && 2 * i + 1 < max; i++) {
        pv[2 * i] = al1params[i].param;
        pv[2 * i + 1] = al1params[i].type;
    }
    return 2 * NAL1PARAMS;
}

char *ALgetname(long dev, long param)
{
    long i;

    (void)dev;
    for (i = 0; i < NAL1PARAMS; i++)
        if (al1params[i].param == param)
            return (char *)al1params[i].name;
    return "";
}

/* No underflow or overflow is ever reported. */
int ALgetstatus(ALport p, long *pv, long n)
{
    long i;

    CHECK_PORT(p, -1);
    if (!pv || n < 0 || (n & 1)) {
        alerror(AL_BAD_PVBUFFER, "ALgetstatus: bad param/value buffer");
        return -1;
    }
    for (i = 0; i < n; i += 2)
        pv[i + 1] = 0;
    return 0;
}
