/* wmaudsdk.dll stand-in: the Windows Media Audio SDK's reader, for TD6's music (Music\*.WMA,
   WMA 2, 44.1 kHz stereo), decoded by FFmpeg's libavcodec.

   What the game does (0x40e6c0, 0x40cfa0 start a track; 0x40e8f0 stops it; its callback object
   has the vtable at 0x46d390):
   - WMAudioCreateReader (ordinal 1)(file name (wide), callback, &reader, 0).
   - reader +0xc GetAttributeByName(L"Duration", &type, value, &len): the low dword is compared
     with sample times in milliseconds, so it is given in milliseconds.
   - reader +0x1c Start: the reader then calls callback +0xc OnSample(data, bytes, ms time) from
     its own thread, and +0x10 OnStatus(status, hr, 0) (3 = end of the file, then the game stops).
     The game copies the samples into its 8-second ring and, once 6 seconds are in, asks for
     reader +0x18 GetOutputFormat(WAVEFORMATEX, 18) and starts a DirectSound buffer it refills
     from the ring itself.
   - reader +0x20 Stop, +8 Release.
   So the samples come as the real reader gave them: the first 6.5 seconds at once, then in real
   time, 6.5 seconds ahead of what has played. In --test runs (with TD6_TEST_MUSIC=1) they come at
   frame ends on the virtual clock (wma_run_due). */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "runtime.h"

#define RATE 44100
#define BYTES_PER_SEC (RATE * 4)
#define CHUNK_FRAMES 4410                /* 100 ms a call */
#define LEAD_MS 6500.0

typedef struct {
    uint32_t obj, callback;
    int refs;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int as, eof;
    int16_t *pend;                       /* decoded, not yet delivered (stereo frames) */
    size_t npend, cappend;
    uint64_t delivered;                  /* frames given to the game */
    double t0;                           /* when delivery started (ms) */
    uint32_t duration_ms;
    uint32_t chunk;                      /* program memory for one OnSample's data */
    int running, done;
    volatile int stop;
    pthread_t thread;
    CPU *cpu;
    pthread_mutex_t lock;
    char name[64];
} Reader;

#define MAX_READERS 4
static Reader *readers[MAX_READERS];
static ComClass cls_reader;

/* More decoded frames into r->pend; 0 at the end of the file */
static int decode_more(Reader *r)
{
    while (!r->eof) {
        int e = avcodec_receive_frame(r->dec, r->frame);
        if (e == 0) {
            if (!r->swr) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&r->swr, &stereo, AV_SAMPLE_FMT_S16, RATE, &r->frame->ch_layout,
                                        r->frame->format, r->frame->sample_rate, 0, NULL) < 0 || swr_init(r->swr) < 0) {
                    r->eof = 1;
                    return 0;
                }
            }
            int out = swr_get_out_samples(r->swr, r->frame->nb_samples);
            if (r->npend + (size_t)out > r->cappend) {
                r->cappend = (r->npend + (size_t)out) * 2;
                r->pend = realloc(r->pend, r->cappend * 4);
            }
            uint8_t *dst = (uint8_t *)(r->pend + 2 * r->npend);
            int got = swr_convert(r->swr, &dst, out, (const uint8_t **)r->frame->extended_data, r->frame->nb_samples);
            if (got > 0) {
                r->npend += (size_t)got;
                return 1;
            }
            continue;
        }
        if (e == AVERROR_EOF) {
            r->eof = 1;
            break;
        }
        if (av_read_frame(r->fmt, r->pkt) < 0) {
            avcodec_send_packet(r->dec, NULL);
            continue;
        }
        if (r->pkt->stream_index == r->as)
            avcodec_send_packet(r->dec, r->pkt);
        av_packet_unref(r->pkt);
    }
    return 0;
}

/* Deliver what is due by `now` (ms); returns 1 while there is more to come */
static int deliver_due(Reader *r, CPU *c, double now)
{
    if (r->t0 < 0)
        r->t0 = now;
    double due_ms = now - r->t0 + LEAD_MS;
    while (!r->stop && (double)r->delivered * 1000.0 / RATE < due_ms) {
        while (r->npend < CHUNK_FRAMES && decode_more(r)) {}
        size_t n = r->npend < CHUNK_FRAMES ? r->npend : CHUNK_FRAMES;
        if (!n) {
            if (!r->done) {
                r->done = 1;
                LOG(LOG_SOUND, "music %s: end of the file after %llu frames", r->name, (unsigned long long)r->delivered);
                guest_call(c, R32(R32(r->callback) + 0x10), 4, r->callback, 3, 0, 0);    /* OnStatus(WMT_EOF) */
            }
            return 0;
        }
        memcpy(gptr(r->chunk), r->pend, n * 4);
        memmove(r->pend, r->pend + 2 * n, (r->npend - n) * 4);
        r->npend -= n;
        uint32_t ms = (uint32_t)(r->delivered * 1000 / RATE);
        r->delivered += n;
        guest_call(c, R32(R32(r->callback) + 0xc), 4, r->callback, r->chunk, (uint32_t)(n * 4), ms);    /* OnSample */
    }
    return 1;
}

static double real_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void *reader_main(void *arg)
{
    Reader *r = arg;
    set_current_cpu(r->cpu);
    fpu_activate(r->cpu);
    while (!r->stop) {
        pthread_mutex_lock(&r->lock);
        int more = deliver_due(r, r->cpu, real_now());
        pthread_mutex_unlock(&r->lock);
        if (!more)
            break;
        struct timespec ts = {0, 20 * 1000000L};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* --test: delivery on the main thread at frame ends, on the virtual clock */
void wma_run_due(double now)
{
    CPU *self = current_cpu();
    for (int i = 0; i < MAX_READERS; i++) {
        Reader *r = readers[i];
        if (!r || !r->running || r->done || r->stop)
            continue;
        set_current_cpu(r->cpu);
        fpu_activate(r->cpu);
        deliver_due(r, r->cpu, now);
        set_current_cpu(self);
        if (self)
            fpu_activate(self);
    }
}

static void reader_stop(Reader *r)
{
    if (!r->running)
        return;
    r->stop = 1;
    if (!deterministic && !pthread_equal(r->thread, pthread_self()))
        pthread_join(r->thread, NULL);
    r->running = 0;
}

static void reader_free(Reader *r)
{
    reader_stop(r);
    for (int i = 0; i < MAX_READERS; i++)
        if (readers[i] == r)
            readers[i] = NULL;
    swr_free(&r->swr);
    av_frame_free(&r->frame);
    av_packet_free(&r->pkt);
    avcodec_free_context(&r->dec);
    avformat_close_input(&r->fmt);
    free(r->pend);
    if (r->chunk)
        gfree(r->chunk);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

METHOD(rd_QueryInterface, 3)
{
    if (ARG(2)) W32(ARG(2), 0);
    return 0x80004002u;                  /* E_NOINTERFACE: the game never asks */
}
METHOD(rd_AddRef, 1)
{
    Reader *r = com_host(ARG(0));
    return (uint32_t)++r->refs;
}
METHOD(rd_Release, 1)
{
    Reader *r = com_host(ARG(0));
    int n = --r->refs;
    if (n <= 0)
        reader_free(r);
    return (uint32_t)(n < 0 ? 0 : n);
}
METHOD(rd_GetAttributeCount, 2)
{
    if (ARG(1)) W16(ARG(1), 1);
    return 0;
}
METHOD(rd_GetAttributeByIndex, 6) { return 0x80004001u; }    /* E_NOTIMPL */
/* (name, &type, value, &length): Duration only */
METHOD(rd_GetAttributeByName, 5)
{
    Reader *r = com_host(ARG(0));
    uint32_t name = ARG(1), typep = ARG(2), value = ARG(3), lenp = ARG(4);
    static const char want[] = "Duration";
    for (int i = 0; i < 9; i++)
        if (R16(name + 2 * i) != (uint16_t)want[i])
            return 0xc00d0bb8u;          /* NS_E_... no such attribute */
    if (typep) W32(typep, 4);            /* WMT_TYPE_QWORD */
    if (value) {
        W32(value, r->duration_ms);
        W32(value + 4, 0);
    }
    if (lenp) W16(lenp, 8);
    return 0;
}
/* (WAVEFORMATEX *, size) */
METHOD(rd_GetOutputFormat, 3)
{
    uint32_t w = ARG(1);
    if (!w || ARG(2) < 16)
        return 0x80070057u;
    W16(w, 1);                           /* WAVE_FORMAT_PCM */
    W16(w + 2, 2);
    W32(w + 4, RATE);
    W32(w + 8, BYTES_PER_SEC);
    W16(w + 12, 4);
    W16(w + 14, 16);
    if (ARG(2) >= 18)
        W16(w + 16, 0);
    return 0;
}
METHOD(rd_Start, 1)
{
    Reader *r = com_host(ARG(0));
    if (r->running)
        return 0;
    r->stop = 0;
    r->t0 = -1;
    r->running = 1;
    LOG(LOG_SOUND, "music %s: start", r->name);
    if (!deterministic)
        pthread_create(&r->thread, NULL, reader_main, r);
    return 0;
}
METHOD(rd_Stop, 1)
{
    Reader *r = com_host(ARG(0));
    LOG(LOG_SOUND, "music %s: stop after %llu frames", r->name, (unsigned long long)r->delivered);
    reader_stop(r);
    return 0;
}
METHOD(rd_Unknown, 1) { return 0x80004001u; }

/* WMAudioCreateReader(file, callback, &reader, reserved) */
API(WMAUDSDK, 1, 4)
{
    uint32_t wname = ARG(0), cb = ARG(1), pp = ARG(2);
    char name[600];
    int k = 0;
    for (; k < (int)sizeof name - 1 && R16(wname + 2 * k); k++)
        name[k] = (char)R16(wname + 2 * k);
    name[k] = 0;
    if (pp) W32(pp, 0);
    /* --test runs have no music unless TD6_TEST_MUSIC=1: the game streams it from a thread of its
       own woken by the sound buffer's notifications, which no virtual clock can make repeatable
       (it carries on as without the WMA codec) */
    if (deterministic && !(getenv("TD6_TEST_MUSIC") && atoi(getenv("TD6_TEST_MUSIC")))) {
        LOG(LOG_SOUND, "WMAudioCreateReader(\"%s\"): no music in test runs", name);
        return 0xc00d0bb8u;
    }
    char host[2048];
    if (!path_to_host(name, host, sizeof host, 0)) {
        LOG(LOG_ALWAYS, "WMAudioCreateReader(\"%s\"): no such file", name);
        return 0x80070002u;
    }
    int slot = -1;
    for (int i = 0; i < MAX_READERS && slot < 0; i++)
        if (!readers[i])
            slot = i;
    if (slot < 0)
        return 0x8007000eu;
    Reader *r = calloc(1, sizeof *r);
    pthread_mutex_init(&r->lock, NULL);
    r->callback = cb;
    r->refs = 1;
    r->t0 = -1;
    const char *bs = strrchr(name, '\\');
    snprintf(r->name, sizeof r->name, "%s", bs ? bs + 1 : name);
    if (avformat_open_input(&r->fmt, host, NULL, NULL) < 0 || avformat_find_stream_info(r->fmt, NULL) < 0 ||
        (r->as = av_find_best_stream(r->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0)) < 0)
        goto fail;
    const AVCodec *codec = avcodec_find_decoder(r->fmt->streams[r->as]->codecpar->codec_id);
    if (!codec)
        goto fail;
    r->dec = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(r->dec, r->fmt->streams[r->as]->codecpar) < 0 || avcodec_open2(r->dec, codec, NULL) < 0)
        goto fail;
    r->pkt = av_packet_alloc();
    r->frame = av_frame_alloc();
    r->duration_ms = r->fmt->duration > 0 ? (uint32_t)(r->fmt->duration / 1000) : 0;
    r->chunk = galloc(CHUNK_FRAMES * 4);
    r->cpu = cpu_new(0x40000);
    r->obj = com_new(&cls_reader, r);
    readers[slot] = r;
    if (pp) W32(pp, r->obj);
    LOG(LOG_ALWAYS, "music %s: %u ms", r->name, r->duration_ms);
    return 0;
fail:
    LOG(LOG_ALWAYS, "WMAudioCreateReader(\"%s\"): cannot decode it", name);
    r->refs = 0;
    reader_free(r);
    return 0xc00d0bb8u;
}

void wma_init(void)
{
    /* the slots the game calls: +0xc GetAttributeByName, +0x18 GetOutputFormat, +0x1c Start, +0x20 Stop */
    static void (*const m[])(CPU *) = {rd_QueryInterface, rd_AddRef, rd_Release, rd_GetAttributeByName,
                                       rd_GetAttributeCount, rd_GetAttributeByIndex, rd_GetOutputFormat,
                                       rd_Start, rd_Stop, rd_Unknown, rd_Unknown, rd_Unknown, rd_Unknown};
    static const char *const n[] = {"QueryInterface", "AddRef", "Release", "GetAttributeByName",
                                    "GetAttributeCount", "GetAttributeByIndex", "GetOutputFormat", "Start",
                                    "Stop", "Method9", "Method10", "Method11", "Method12"};
    com_class_init(&cls_reader, "IWMAudioReader", (int)(sizeof m / sizeof *m), m, n);
}
