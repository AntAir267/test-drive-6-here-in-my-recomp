/* binkw32.dll stand-in: TD6's movies (Movies\*.bik, Bink 1 "f") decoded by FFmpeg's libavcodec.

   What the game does (0x40c480-0x40cd20):
   - BinkSetSoundSystem(BinkOpenDirectSound, its IDirectSound) once; the soundtrack plays through
     the runtime's mixer here (sound_stream_start), whatever system it names.
   - BinkOpen(path, 0) -> a BINK record it reads: +0 width, +4 height, +0xc the height shown
     (twice the height for line-doubled files: header flag 0x10000000, FFCars.bik), +0x10 frames,
     +0x14 the current frame (1-based).
   - Each frame: BinkWait (1: not yet time), BinkDoFrame, Lock the target surface, BinkCopyToBuffer
     (dest, pitch, rows, x, y, format | BINKCOPYALL), Unlock, Blt it; then BinkNextFrame, or at
     frame (frames - 1) BinkClose or, for the menus' looping previews (240 x 180), BinkGoto(1).
     Full-screen movies go straight into the back buffer at an offset; line-doubled and preview
     movies into a surface of their own that is Blt'ed (stretched) where they show.
   - Copy formats (the surface's): 1 32-bit X8R8G8B8, 2 RGB555, 3 RGB565, 4 RGB655, 5 RGB664.

   Frames are timed by the runtime's clock from the first BinkDoFrame (BinkWait), the sound starts
   with it; a frame the game is late for is still shown (none are dropped). */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "sound.h"

#define MAX_MOVIES 8

typedef struct {
    uint32_t rec;                       /* the BINK record in program memory */
    AVFormatContext *fmt;
    AVCodecContext *vdec;
    int vs;
    AVFrame *frame, *rgb;
    AVPacket *pkt;
    struct SwsContext *sws;
    int w, h, frames, cur, have;        /* cur: the frame the game is on (1-based); have: decoded */
    double fps, t0;                     /* t0: when frame 1 was due (ms), < 0 until the first DoFrame */
    int16_t *pcm;
    uint32_t pcm_frames;
    int ch, rate, voice;
    char name[64];
} Movie;

static Movie movies[MAX_MOVIES];
static int sound_on;

static Movie *movie_of(uint32_t rec)
{
    for (int i = 0; i < MAX_MOVIES; i++)
        if (movies[i].rec && movies[i].rec == rec)
            return &movies[i];
    return NULL;
}

static void put_frame_num(Movie *m)
{
    W32(m->rec + 0x18, R32(m->rec + 0x14));
    W32(m->rec + 0x14, (uint32_t)m->cur);
}

/* The whole soundtrack, decoded at open into 16-bit PCM at its own rate (a few MB at most) */
static void decode_audio(Movie *m, const char *path)
{
    AVFormatContext *f = NULL;
    AVCodecContext *ad = NULL;
    SwrContext *swr = NULL;
    if (avformat_open_input(&f, path, NULL, NULL) < 0)
        return;
    if (avformat_find_stream_info(f, NULL) < 0)
        goto done;
    int as = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (as < 0)
        goto done;
    const AVCodec *codec = avcodec_find_decoder(f->streams[as]->codecpar->codec_id);
    ad = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!ad || avcodec_parameters_to_context(ad, f->streams[as]->codecpar) < 0 || avcodec_open2(ad, codec, NULL) < 0)
        goto done;
    int ch = ad->ch_layout.nb_channels > 1 ? 2 : 1, rate = 0;
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, ch);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    size_t cap = 0, n = 0;
    int16_t *pcm = NULL;
    int eof = 0;
    while (!eof) {
        if (av_read_frame(f, pkt) < 0) {
            eof = 1;
            avcodec_send_packet(ad, NULL);
        } else {
            if (pkt->stream_index == as)
                avcodec_send_packet(ad, pkt);
            av_packet_unref(pkt);
        }
        while (avcodec_receive_frame(ad, fr) == 0) {
            if (!swr) {                 /* from the first frame: what the decoder really gives */
                rate = fr->sample_rate;
                if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, rate, &fr->ch_layout, fr->format, rate,
                                        0, NULL) < 0 || swr_init(swr) < 0) {
                    eof = 1;
                    break;
                }
            }
            int out = swr_get_out_samples(swr, fr->nb_samples);
            if (n + (size_t)out > cap) {
                cap = (n + (size_t)out) * 2 + 4096;
                pcm = realloc(pcm, cap * (size_t)ch * sizeof *pcm);
            }
            uint8_t *dst = (uint8_t *)(pcm + n * (size_t)ch);
            int got = swr_convert(swr, &dst, out, (const uint8_t **)fr->extended_data, fr->nb_samples);
            if (got > 0)
                n += (size_t)got;
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    m->pcm = pcm;
    m->pcm_frames = (uint32_t)n;
    m->ch = ch;
    m->rate = rate;
done:
    swr_free(&swr);
    avcodec_free_context(&ad);
    avformat_close_input(&f);
}

static void movie_free(Movie *m)
{
    if (m->voice >= 0)
        sound_stream_stop(m->voice);
    sws_freeContext(m->sws);
    av_frame_free(&m->frame);
    av_frame_free(&m->rgb);
    av_packet_free(&m->pkt);
    avcodec_free_context(&m->vdec);
    avformat_close_input(&m->fmt);
    free(m->pcm);
    if (m->rec)
        gfree(m->rec);
    memset(m, 0, sizeof *m);
    m->voice = -1;
}

/* Decode until frame `want` (1-based) is in m->frame (back to the start for an earlier one) */
static int decode_to(Movie *m, int want)
{
    if (want < m->have || m->have == 0) {
        if (m->have && av_seek_frame(m->fmt, m->vs, 0, AVSEEK_FLAG_BACKWARD) < 0)
            return 0;
        if (m->have)
            avcodec_flush_buffers(m->vdec);
        m->have = 0;
    }
    while (m->have < want) {
        int r = avcodec_receive_frame(m->vdec, m->frame);
        if (r == 0) {
            m->have++;
            continue;
        }
        if (r == AVERROR_EOF)
            return m->have > 0;
        if (av_read_frame(m->fmt, m->pkt) < 0) {
            avcodec_send_packet(m->vdec, NULL);
            continue;
        }
        if (m->pkt->stream_index == m->vs)
            avcodec_send_packet(m->vdec, m->pkt);
        av_packet_unref(m->pkt);
    }
    return 1;
}

API(BINKW32, _BinkSetSoundSystem_8, 2)
{
    sound_on = 1;                       /* (BinkOpenDirectSound and the game's IDirectSound) */
    LOG(LOG_API, "BinkSetSoundSystem(%08x, %08x)", ARG(0), ARG(1));
    return 1;
}

/* The game passes BinkOpenDirectSound's address to BinkSetSoundSystem and never calls it */
API(BINKW32, _BinkOpenDirectSound_4, 1) { return 0; }

API(BINKW32, _BinkOpen_8, 2)
{
    const char *name = gstr(ARG(0));
    char host[2048];
    if (!path_to_host(name, host, sizeof host, 0)) {
        LOG(LOG_ALWAYS, "BinkOpen(\"%s\"): no such file", name);
        return 0;
    }
    Movie *m = NULL;
    for (int i = 0; i < MAX_MOVIES && !m; i++)
        if (!movies[i].rec)
            m = &movies[i];
    if (!m)
        return 0;
    memset(m, 0, sizeof *m);
    m->voice = -1;
    uint8_t hdr[44];
    FILE *fp = fopen(host, "rb");
    if (!fp || fread(hdr, 1, sizeof hdr, fp) != sizeof hdr || memcmp(hdr, "BIK", 3)) {
        if (fp) fclose(fp);
        LOG(LOG_ALWAYS, "BinkOpen(\"%s\"): not a Bink file", name);
        return 0;
    }
    fclose(fp);
    uint32_t flags, fn, fd;
    memcpy(&m->frames, hdr + 8, 4);
    memcpy(&fn, hdr + 0x1c, 4);
    memcpy(&fd, hdr + 0x20, 4);
    memcpy(&flags, hdr + 0x24, 4);
    if (avformat_open_input(&m->fmt, host, NULL, NULL) < 0 || avformat_find_stream_info(m->fmt, NULL) < 0)
        goto fail;
    m->vs = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (m->vs < 0)
        goto fail;
    AVStream *st = m->fmt->streams[m->vs];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        goto fail;
    m->vdec = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(m->vdec, st->codecpar) < 0 || avcodec_open2(m->vdec, codec, NULL) < 0)
        goto fail;
    m->w = st->codecpar->width;
    m->h = st->codecpar->height;
    m->fps = fn && fd ? (double)fn / fd : 30.0;
    m->frame = av_frame_alloc();
    m->rgb = av_frame_alloc();
    m->pkt = av_packet_alloc();
    m->rgb->format = AV_PIX_FMT_BGR0;
    m->rgb->width = m->w;
    m->rgb->height = m->h;
    if (av_frame_get_buffer(m->rgb, 32) < 0)
        goto fail;
    memset(m->rgb->data[0], 0, (size_t)m->rgb->linesize[0] * m->h);
    if (sound_on)
        decode_audio(m, host);
    m->rec = galloc(0x100);
    memset(gptr(m->rec), 0, 0x100);
    W32(m->rec + 0x00, (uint32_t)m->w);
    W32(m->rec + 0x04, (uint32_t)m->h);
    W32(m->rec + 0x08, (uint32_t)m->w);
    W32(m->rec + 0x0c, (uint32_t)(flags & 0x10000000u ? 2 * m->h : m->h));
    W32(m->rec + 0x10, (uint32_t)m->frames);
    W32(m->rec + 0x1c, fn);
    W32(m->rec + 0x20, fd);
    m->cur = 1;
    m->t0 = -1;
    put_frame_num(m);
    const char *bs = strrchr(name, '\\');
    snprintf(m->name, sizeof m->name, "%s", bs ? bs + 1 : name);
    LOG(LOG_ALWAYS, "movie %s: %dx%d, %d frames at %.2f fps%s, sound %u frames", m->name, m->w, m->h, m->frames,
        m->fps, flags & 0x10000000u ? ", line-doubled" : "", m->pcm_frames);
    return m->rec;
fail:
    LOG(LOG_ALWAYS, "BinkOpen(\"%s\"): cannot decode it", name);
    movie_free(m);
    return 0;
}

API(BINKW32, _BinkClose_4, 1)
{
    Movie *m = movie_of(ARG(0));
    if (m) {
        LOG(LOG_API, "BinkClose(%s) at frame %d", m->name, m->cur);
        movie_free(m);
    }
    return 0;
}

/* 1 while it is too early for the current frame */
API(BINKW32, _BinkWait_4, 1)
{
    Movie *m = movie_of(ARG(0));
    if (!m || m->t0 < 0)
        return 0;
    return now_ms() < m->t0 + (m->cur - 1) * 1000.0 / m->fps;
}

API(BINKW32, _BinkDoFrame_4, 1)
{
    Movie *m = movie_of(ARG(0));
    if (!m)
        return 0;
    if (m->t0 < 0) {
        m->t0 = now_ms() - (m->cur - 1) * 1000.0 / m->fps;
        if (m->pcm && m->cur == 1)
            m->voice = sound_stream_start(m->pcm, m->pcm_frames, m->ch, m->rate);
    }
    if (!decode_to(m, m->cur))
        return 0;
    m->sws = sws_getCachedContext(m->sws, m->w, m->h, m->frame->format, m->w, m->h, AV_PIX_FMT_BGR0,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (m->sws)
        sws_scale(m->sws, (const uint8_t *const *)m->frame->data, m->frame->linesize, 0, m->h, m->rgb->data,
                  m->rgb->linesize);
    return 0;
}

API(BINKW32, _BinkNextFrame_4, 1)
{
    Movie *m = movie_of(ARG(0));
    if (m && m->cur < m->frames) {
        m->cur++;
        put_frame_num(m);
    }
    return 0;
}

/* Back to a frame (the previews loop to frame 1): the timing and the sound start again from it */
API(BINKW32, _BinkGoto_12, 3)
{
    Movie *m = movie_of(ARG(0));
    if (!m)
        return 0;
    int f = (int)ARG(1);
    if (f < 1) f = 1;
    if (f > m->frames) f = m->frames;
    m->cur = f;
    m->t0 = now_ms() - (f - 1) * 1000.0 / m->fps;
    if (m->voice >= 0) {
        sound_stream_stop(m->voice);
        m->voice = -1;
    }
    if (m->pcm && f == 1)
        m->voice = sound_stream_start(m->pcm, m->pcm_frames, m->ch, m->rate);
    put_frame_num(m);
    return 0;
}

/* (bink, dest, pitch, rows, x, y, flags): the decoded frame into the game's locked surface */
API(BINKW32, _BinkCopyToBuffer_28, 7)
{
    Movie *m = movie_of(ARG(0));
    if (!m)
        return 0;
    uint32_t dest = ARG(1), pitch = ARG(2), rows = ARG(3), x = ARG(4), y = ARG(5), kind = ARG(6) & 0xff;
    uint32_t bpp = kind == 1 ? 4 : 2;
    for (int row = 0; row < m->h && (uint32_t)row + y < rows; row++) {
        const uint8_t *s = m->rgb->data[0] + (size_t)row * m->rgb->linesize[0];
        uint8_t *d = (uint8_t *)gptr(dest + (row + y) * pitch + x * bpp);
        if (kind == 1) {
            memcpy(d, s, (size_t)m->w * 4);
            continue;
        }
        uint16_t *o = (uint16_t *)d;
        for (int i = 0; i < m->w; i++) {
            uint32_t b = s[4 * i], g = s[4 * i + 1], r = s[4 * i + 2], v;
            switch (kind) {
            case 2: v = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;          /* 555 */
            case 4: v = (r >> 2) << 10 | (g >> 3) << 5 | b >> 3; break;          /* 655 */
            case 5: v = (r >> 2) << 10 | (g >> 2) << 4 | b >> 4; break;          /* 664 */
            default: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;         /* 565 */
            }
            o[i] = (uint16_t)v;
        }
    }
    return 0;
}

void bink_init(void)
{
    for (int i = 0; i < MAX_MOVIES; i++)
        movies[i].voice = -1;
    av_log_set_level(AV_LOG_ERROR);
}

/* Escape or a controller button while a movie plays (win.c, pad.c); the game's key poll takes it
   once (user32.c) */
static int skip_pending;
void video_skip_press(void) { skip_pending = 1; }
int video_skip_taken(void)
{
    int r = skip_pending;
    skip_pending = 0;
    return r;
}
