/* DirectSound stand-in (DESIGN.md point 5): IDirectSound and IDirectSoundBuffer with the
   DirectX 7 vtable layouts, over our own mixer. Buffer memory is program memory handed out by
   Lock; the mixer reads it in place. How play positions move (by the clock now, by the audio
   device's consumption later) is described in sound.h.

   What the game does (Game.exe 0x492e70 and neighbours): CoCreateInstance(CLSID_DirectSound)
   + Initialize(NULL), SetCooperativeLevel(PRIORITY), primary buffer {PRIMARYBUFFER} with
   SetFormat 22050 Hz 16-bit stereo and Play(LOOPING); DirectSoundCreate + EXCLUSIVE only as a
   fallback. SFX: STATIC|CTRLFREQUENCY|CTRLPAN|CTRLVOLUME|GETCURRENTPOSITION2 buffers filled
   by one Lock, up to 32 DuplicateSoundBuffer copies each, played with Stop / SetVolume /
   SetPan / SetFrequency / SetCurrentPosition(0) / Play(0 or LOOPING), polled by GetStatus.
   Music: a 1.2 s CTRLFREQUENCY|CTRLPAN|CTRLVOLUME buffer played looping and refilled from a
   100 ms timer thread: GetCurrentPosition, then Lock/Unlock of [last play cursor, play
   cursor). */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "sound.h"

/* ------------------------------------------------------------------ constants */
#define DS_OK                     0u
#define E_NOTIMPL                 0x80004001u
#define E_NOINTERFACE             0x80004002u
#define CLASS_E_NOAGGREGATION     0x80040110u
#define REGDB_E_CLASSNOTREG       0x80040154u
#define E_INVALIDARG              0x80070057u
#define DSHR(code)                (0x88780000u | (code))
#define DSERR_CONTROLUNAVAIL      DSHR(30)
#define DSERR_INVALIDPARAM        E_INVALIDARG
#define DSERR_INVALIDCALL         DSHR(50)
#define DSERR_PRIOLEVELNEEDED     DSHR(70)
#define DSERR_BADFORMAT           DSHR(100)
#define DSERR_UNSUPPORTED         E_NOTIMPL
#define DSERR_ALREADYINITIALIZED  DSHR(130)
#define DSERR_NOAGGREGATION       CLASS_E_NOAGGREGATION
#define DSERR_UNINITIALIZED       DSHR(170)

#define DSBCAPS_PRIMARYBUFFER     0x00000001u
#define DSBCAPS_LOCHARDWARE       0x00000004u
#define DSBCAPS_LOCSOFTWARE       0x00000008u
#define DSBCAPS_CTRL3D            0x00000010u
#define DSBCAPS_CTRLFREQUENCY     0x00000020u
#define DSBCAPS_CTRLPAN           0x00000040u
#define DSBCAPS_CTRLVOLUME        0x00000080u
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100u
#define DSBCAPS_CTRLFX            0x00000200u
#define DSBCAPS_LOCDEFER          0x00040000u
#define DSBPLAY_LOOPING           1u
#define DSBSTATUS_PLAYING         1u
#define DSBSTATUS_LOOPING         4u
#define DSBSTATUS_LOCSOFTWARE     0x10u
#define DSBLOCK_FROMWRITECURSOR   1u
#define DSBLOCK_ENTIREBUFFER      2u
#define DSSCL_PRIORITY            2u
#define DSSCL_WRITEPRIMARY        4u
#define DSSPEAKER_STEREO_WIDE     0x00140004u   /* DSSPEAKER_COMBINED(STEREO, GEOMETRY_WIDE) */
#define FREQ_MIN                  100u
#define FREQ_MAX                  200000u       /* DX9-era runtimes; DX7's header says 100000 */
#define DSBSIZE_MIN               4u
#define DSBSIZE_MAX               0x0fffffffu
#define VOL_MIN                   (-10000)

/* ------------------------------------------------------------------ GUIDs */
typedef struct { const char *name; uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
#define DSGUID(n, a) {n, a, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}}
static const Guid known_guids[] = {
    {"IUnknown", 0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}},
    DSGUID("IDirectSound", 0x279afa83),
    DSGUID("IDirectSoundBuffer", 0x279afa85),
    {"CLSID_DirectSound", 0x47d4d946, 0x62e8, 0x11cf, {0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0, 0}},
    /* the rest only to name them in the log */
    DSGUID("IDirectSound3DListener", 0x279afa84),
    DSGUID("IDirectSound3DBuffer", 0x279afa86),
    {"IDirectSound8", 0xc50a7e93, 0xf395, 0x4834, {0x9e, 0xf6, 0x7f, 0xa9, 0x9d, 0xe5, 0x09, 0x66}},
    {"IDirectSoundBuffer8", 0x6825a449, 0x7524, 0x4d82, {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e}},
    {"IDirectSoundNotify", 0xb0210783, 0x89cd, 0x11d0, {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16}},
    {"IKsPropertySet", 0x31efac30, 0x515c, 0x11d0, {0xa9, 0xaa, 0x00, 0xaa, 0x00, 0x61, 0xbe, 0x93}},
    {"CLSID_DirectSound8", 0x3901cc3f, 0x84b5, 0x4fa4, {0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b}},
};
enum { G_IUNKNOWN, G_IDIRECTSOUND, G_IDIRECTSOUNDBUFFER, G_CLSID_DIRECTSOUND };
static const uint8_t ksdataformat_pcm[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};

static int is_guid(uint32_t a, int which)
{
    const Guid *g = &known_guids[which];
    return guid_eq(a, g->d1, g->d2, g->d3, g->d4);
}

static const char *guid_str(uint32_t a, char *s, size_t n)
{
    if (!a)
        return "NULL";
    for (size_t i = 0; i < sizeof known_guids / sizeof *known_guids; i++)
        if (is_guid(a, (int)i))
            return known_guids[i].name;
    snprintf(s, n, "{" GUID_FMT "}", GUID_ARGS(a));
    return s;
}

/* ------------------------------------------------------------------ state */
typedef struct Pcm {              /* sample memory shared by a buffer and its duplicates */
    int refs;
    uint32_t mem, bytes;
} Pcm;

typedef struct Fmt {
    uint8_t raw[40];              /* WAVEFORMATEX(+extensible part), program layout */
    uint32_t len, channels, bits, block, rate;
} Fmt;

typedef struct DS DS;
typedef struct Buf {
    struct Buf *next;
    DS *ds;
    uint32_t obj;                 /* the program's interface pointer */
    int id, refs, primary;
    uint32_t flags;               /* DSBCAPS_* as created */
    Pcm *pcm;                     /* NULL for the primary buffer */
    Fmt fmt;
    uint32_t channels, bits, block, rate;   /* copies of fmt's, used by the mixer */
    uint32_t bytes, frames;
    uint32_t freq;                /* playback frequency (SetFrequency) */
    int32_t volume, pan;          /* millibels */
    float gain_l, gain_r;
    int playing, looping;
    uint64_t pos;                 /* play position in frames, 32.32 fixed point */
    float *hist;                  /* the HIST frames before hist_end as the mixer last saw them */
    uint32_t hist_end;
    int hist_ok;
    uint32_t notify_obj;          /* its IDirectSoundNotify (0 until asked for) */
    uint32_t obj3d;               /* its IDirectSound3DBuffer (CTRL3D buffers; 0 until asked for) */
    float p3[16], p3_pending[16]; /* DS3DBUFFER (position 1-3, velocity 4-6, cone 7-12, min 13, max 14,
                                     mode 15) as floats/words; the pending copy until a commit */
    int p3_dirty;
    float gain3_l, gain3_r;       /* what 3D placement makes of the gains (1 without) */
    float doppler;                /* ... and of the frequency */
    int nnotify;                  /* SetNotificationPositions: byte offsets and the events they set */
    uint32_t notify_off[32], notify_ev[32];
} Buf;

struct DS {
    uint32_t obj;
    int refs, initialized;
    uint32_t level, hwnd, speaker;
    uint8_t pfmt[18];             /* primary format; Windows' default is 22 kHz 8-bit stereo */
    Buf *primary;
    uint32_t listener;            /* IDirectSound3DListener (from the primary buffer) */
    float ls[16], ls_pending[16]; /* DS3DLISTENER: position 1-3, velocity 4-6, front 7-9, top 10-12,
                                     distance 13, rolloff 14, doppler 15 */
    int ls_dirty;
};

static pthread_mutex_t snd_lock = PTHREAD_MUTEX_INITIALIZER;
static Buf *bufs;                 /* every live buffer of every DirectSound object */
static int next_id = 1;
static ComClass ds_cls, buf_cls, dead_ds_cls, dead_buf_cls, notify_cls, ls_cls, b3_cls;
static int verbose;
static float master_l = 1.0f, master_r = 1.0f;   /* the primary buffer's volume/pan */

/* who steps the positions (sound.h) */
static double (*clock_fn)(void) = now_ms;
static int pull_internal, pull_external;         /* WAV writer thread / attached device */
static double chunk_ms_internal, chunk_ms_external;
static double clock_anchor;
static uint64_t clock_frames;
static uint64_t out_frames;                      /* frames stepped so far, for the primary cursor */
static int out_rate = SOUND_CLOCK_RATE;

#define VLOG(...) do { if (verbose) LOG(LOG_SOUND, __VA_ARGS__); } while (0)
#define FAIL(hr, ...) do { LOG(LOG_SOUND, __VA_ARGS__); return (hr); } while (0)

static void wav_tap(const float *out, int frames, int rate);

/* A COM method whose body runs under the sound lock. */
#define SMETHOD(fname, nargs) \
    static uint32_t fname##_locked(CPU *c); \
    METHOD(fname, nargs) \
    { \
        pthread_mutex_lock(&snd_lock); \
        uint32_t _r = fname##_locked(c); \
        pthread_mutex_unlock(&snd_lock); \
        return _r; \
    } \
    static uint32_t fname##_locked(CPU *c)

/* ------------------------------------------------------------------ log helpers */
static const char *caps_str(uint32_t f, char *s, size_t n)
{
    static const struct { uint32_t bit; const char *name; } t[] = {
        {0x1, "PRIMARYBUFFER"}, {0x2, "STATIC"}, {0x4, "LOCHARDWARE"}, {0x8, "LOCSOFTWARE"},
        {0x10, "CTRL3D"}, {0x20, "CTRLFREQUENCY"}, {0x40, "CTRLPAN"}, {0x80, "CTRLVOLUME"},
        {0x100, "CTRLPOSITIONNOTIFY"}, {0x200, "CTRLFX"}, {0x4000, "STICKYFOCUS"},
        {0x8000, "GLOBALFOCUS"}, {0x10000, "GETCURRENTPOSITION2"},
        {0x20000, "MUTE3DATMAXDISTANCE"}, {0x40000, "LOCDEFER"}};
    size_t k = 0;
    s[0] = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t && k < n; i++)
        if (f & t[i].bit) {
            k += snprintf(s + k, n - k, "%s%s", k ? "|" : "", t[i].name);
            f &= ~t[i].bit;
        }
    if (f && k < n)
        k += snprintf(s + k, n - k, "%s0x%x", k ? "|" : "", f);
    if (!k)
        snprintf(s, n, "0");
    return s;
}

static const char *wfx_str(uint32_t wfx, char *s, size_t n)
{
    if (!wfx)
        return "no format";
    snprintf(s, n, "tag %u, %u ch, %u Hz, %u-bit, align %u, %u B/s", R16(wfx), R16(wfx + 2),
             R32(wfx + 4), R16(wfx + 14), R16(wfx + 12), R32(wfx + 8));
    return s;
}

static const char *buf_str(const Buf *b, char *s, size_t n)
{
    if (b->primary)
        snprintf(s, n, "#%d primary", b->id);
    else
        snprintf(s, n, "#%d", b->id);
    return s;
}
#define BUFNAME(b) buf_str((b), (char[32]){0}, 32)

/* ------------------------------------------------------------------ volume and pan */
static float gain_of(int32_t mb)
{
    if (mb <= VOL_MIN)
        return 0.0f;             /* -100 dB is silence in DirectSound */
    return (float)pow(10.0, mb / 2000.0);
}

/* DirectSound's pan law: the far channel is attenuated by the pan value, the near one not. */
static void place_3d(Buf *b);

static void update_gains(Buf *b)
{
    if (b->flags & DSBCAPS_CTRL3D && !b->primary) {     /* 3D placement takes the pan's place */
        float g = gain_of(b->volume);
        b->gain_l = g * b->gain3_l;
        b->gain_r = g * b->gain3_r;
        return;
    }
    b->gain_l = gain_of(b->volume - (b->pan > 0 ? b->pan : 0));
    b->gain_r = gain_of(b->volume + (b->pan < 0 ? b->pan : 0));
    if (b->primary) {
        master_l = b->gain_l;
        master_r = b->gain_r;
    }
}

/* ------------------------------------------------------------------ resampling
   Windowed sinc (Kaiser, about 70 dB image rejection) on a polyphase table when the buffer
   plays at or below the output rate; above it, the same kernel stretched so it also filters
   at the output's Nyquist. A buffer at exactly the output rate is copied sample for sample. */
#define KN 16                       /* kernel half-width in input frames */
#define KTAPS (2 * KN)
#define KPH_BITS 9
#define KPH (1 << KPH_BITS)
#define KPH_SHIFT (32 - KPH_BITS)
#define KRES 512                    /* table points per input frame for the stretched kernel */
#define KCUT 0.90                   /* cutoff, relative to the lower Nyquist */
#define KBETA 7.0
#define KMAXHALF 128                /* bound on the stretched kernel (only absurd ratios hit it) */
#define ONE (1ull << 32)
static float ptab[KPH + 1][KTAPS];
static float ktab[KN * KRES + 2];

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 64; k++) {
        double q = x / (2 * k);
        t *= q * q;
        s += t;
        if (t < s * 1e-17)
            break;
    }
    return s;
}

static double kernel(double d)
{
    d = fabs(d);
    if (d >= KN)
        return 0;
    double x = d / KN;
    double w = bessel_i0(KBETA * sqrt(1 - x * x)) / bessel_i0(KBETA);
    double s = d < 1e-9 ? KCUT : sin(M_PI * KCUT * d) / (M_PI * d);
    return s * w;
}

static void build_kernel(void)
{
    for (int i = 0; i <= KN * KRES; i++)
        ktab[i] = (float)kernel((double)i / KRES);
    ktab[KN * KRES + 1] = 0;
    for (int p = 0; p <= KPH; p++) {
        double f = (double)p / KPH, sum = 0, row[KTAPS];
        for (int k = 0; k < KTAPS; k++)
            sum += row[k] = kernel(k - (KN - 1) - f);
        for (int k = 0; k < KTAPS; k++)
            ptab[p][k] = (float)(row[k] / sum);
    }
}

/* The kernel reads up to KMAXHALF frames behind the play position, which the game may already
   have refilled (the music streamer writes everything behind the play cursor). So, like a real
   mixer that has consumed them, each voice keeps its own copy of the frames just behind the
   position where its last chunk ended. Not for tiny buffers, where the copy could alias. */
#define HIST KMAXHALF
#define HIST_MIN_FRAMES (4 * HIST)

static inline int hist_get(const Buf *b, int64_t w, float *l, float *r)
{
    int64_t back = (int64_t)b->hist_end - w;
    if (b->looping && back <= 0)
        back += b->frames;
    if (back < 1 || back > HIST)
        return 0;
    const float *h = b->hist + 2 * (HIST - back);
    *l = h[0];
    *r = h[1];
    return 1;
}

static inline void frame_at(const Buf *b, const uint8_t *base, int64_t j, int use_hist, float *l, float *r)
{
    if ((uint64_t)j >= b->frames) {
        if (!b->looping) {
            *l = *r = 0;
            return;
        }
        j %= (int64_t)b->frames;
        if (j < 0)
            j += b->frames;
    }
    if (use_hist && hist_get(b, j, l, r))
        return;
    const uint8_t *p = base + (size_t)j * b->block;
    if (b->bits == 16) {
        int16_t s;
        memcpy(&s, p, 2);
        *l = s * (1.0f / 32768);
        if (b->channels == 2)
            memcpy(&s, p + 2, 2);
        *r = s * (1.0f / 32768);
    } else {
        *l = ((int)p[0] - 128) * (1.0f / 128);
        *r = ((int)p[b->channels - 1] - 128) * (1.0f / 128);
    }
}

static void gather(const Buf *b, const uint8_t *base, int64_t j0, int n, int use_hist, float *l, float *r)
{
    if (use_hist || j0 < 0 || j0 + n > (int64_t)b->frames) {
        for (int k = 0; k < n; k++)
            frame_at(b, base, j0 + k, use_hist, &l[k], &r[k]);
        return;
    }
    const uint8_t *p = base + (size_t)j0 * b->block;
    unsigned second = b->channels == 2 ? b->bits / 8 : 0;
    if (b->bits == 16) {
        for (int k = 0; k < n; k++, p += b->block) {
            int16_t s0, s1;
            memcpy(&s0, p, 2);
            memcpy(&s1, p + second, 2);
            l[k] = s0 * (1.0f / 32768);
            r[k] = s1 * (1.0f / 32768);
        }
    } else {
        for (int k = 0; k < n; k++, p += b->block) {
            l[k] = ((int)p[0] - 128) * (1.0f / 128);
            r[k] = ((int)p[second] - 128) * (1.0f / 128);
        }
    }
}

/* Half-width in input frames of the kernel used at this step. */
static int kernel_half(uint64_t step)
{
    if (step <= ONE)
        return KN;
    double rr = (double)ONE / (double)step;
    if (rr < (double)KN / KMAXHALF)
        rr = (double)KN / KMAXHALF;
    int half = (int)ceil(KN / rr);
    return half > KMAXHALF ? KMAXHALF : half;
}

/* Eight running sums so the compiler can vectorise without reordering float additions. */
static inline float dot_taps(const float *c, const float *x)
{
    float acc[8] = {0};
    for (int k = 0; k < KTAPS; k += 8)
        for (int i = 0; i < 8; i++)
            acc[i] += c[k + i] * x[k + i];
    return ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
}

/* The common case, with fixed-length loops per sample format so they vectorise: the buffer
   plays at or below the output rate and the whole window lies in its memory. */
static void interp_poly(const Buf *b, const uint8_t *p, uint32_t frac, float *ol, float *orr)
{
    const float *c0 = ptab[frac >> KPH_SHIFT], *c1 = c0 + KTAPS;
    float a = (frac & ((1u << KPH_SHIFT) - 1)) * (1.0f / (1u << KPH_SHIFT));
    float c[KTAPS], x[KTAPS], y[KTAPS], scale;
    for (int k = 0; k < KTAPS; k++)
        c[k] = c0[k] + a * (c1[k] - c0[k]);
    int stereo = b->channels == 2;
    if (b->bits == 8) {
        scale = 1.0f / 128;
        if (stereo)
            for (int k = 0; k < KTAPS; k++) {
                x[k] = (float)((int)p[2 * k] - 128);
                y[k] = (float)((int)p[2 * k + 1] - 128);
            }
        else
            for (int k = 0; k < KTAPS; k++)
                x[k] = (float)((int)p[k] - 128);
    } else {
        int16_t s[2 * KTAPS];
        memcpy(s, p, (size_t)KTAPS * b->block);
        scale = 1.0f / 32768;
        if (stereo)
            for (int k = 0; k < KTAPS; k++) {
                x[k] = s[2 * k];
                y[k] = s[2 * k + 1];
            }
        else
            for (int k = 0; k < KTAPS; k++)
                x[k] = s[k];
    }
    *ol = dot_taps(c, x) * scale;
    *orr = stereo ? dot_taps(c, y) * scale : *ol;
}

/* The buffer's signal at fractional frame position pos. */
static void interp(const Buf *b, const uint8_t *base, uint64_t pos, uint64_t step, int half, int use_hist,
                   float *ol, float *orr)
{
    int64_t ip = (int64_t)(pos >> 32);
    uint32_t frac = (uint32_t)pos;
    if (step == ONE && !frac) {
        frame_at(b, base, ip, use_hist, ol, orr);
        return;
    }
    int n = 2 * half;
    int64_t j0 = ip - (half - 1);
    if (step <= ONE && !use_hist && j0 >= 0 && j0 + KTAPS <= (int64_t)b->frames) {
        interp_poly(b, base + (size_t)j0 * b->block, frac, ol, orr);
        return;
    }
    float l[2 * KMAXHALF], r[2 * KMAXHALF], c[2 * KMAXHALF];
    if (step <= ONE) {
        const float *c0 = ptab[frac >> KPH_SHIFT], *c1 = ptab[(frac >> KPH_SHIFT) + 1];
        float a = (frac & ((1u << KPH_SHIFT) - 1)) * (1.0f / (1u << KPH_SHIFT));
        for (int k = 0; k < n; k++)
            c[k] = c0[k] + a * (c1[k] - c0[k]);
    } else {
        double rr = (double)ONE / (double)step;      /* output rate / buffer frequency, < 1 */
        if (rr < (double)KN / half)
            rr = (double)KN / half;                   /* kernel_half's bound */
        double f = frac * (1.0 / 4294967296.0);
        float sum = 0;
        for (int k = 0; k < n; k++) {
            double d = fabs((double)(k - (half - 1)) - f) * rr * KRES;
            int i = (int)d;
            float w = i >= KN * KRES ? 0.0f : ktab[i] + (float)(d - i) * (ktab[i + 1] - ktab[i]);
            c[k] = w;
            sum += w;
        }
        float inv = sum > 0 ? 1.0f / sum : 0.0f;
        for (int k = 0; k < n; k++)
            c[k] *= inv;
    }
    gather(b, base, j0, n, use_hist, l, r);
    float al = 0, ar = 0;
    for (int k = 0; k < n; k++)
        al += c[k] * l[k];
    if (b->channels == 2)
        for (int k = 0; k < n; k++)
            ar += c[k] * r[k];
    else
        ar = al;
    *ol = al;
    *orr = ar;
}

/* ------------------------------------------------------------------ stepping */
static uint64_t step_of(const Buf *b, int rate)
{
    if (b->doppler > 0 && b->doppler != 1.0f)
        return (uint64_t)((double)b->freq * b->doppler * 4294967296.0 / rate);
    return ((uint64_t)b->freq << 32) / (uint64_t)rate;
}

/* A one-shot that reaches its end stops and goes back to the start (as Wine does; the game
   always sets the position before Play anyway). */
static void stop_at_end(Buf *b)
{
    b->playing = 0;
    b->pos = 0;
    b->hist_ok = 0;
}

/* n output frames of stepping without producing samples: identical to what mix_voice does. */
static void advance(Buf *b, uint64_t n, int rate)
{
    if (!b->playing || !n)
        return;
    b->hist_ok = 0;
    unsigned __int128 p = (unsigned __int128)b->pos + (unsigned __int128)step_of(b, rate) * n;
    unsigned __int128 end = (unsigned __int128)b->frames << 32;
    if (p < end)
        b->pos = (uint64_t)p;
    else if (b->looping)
        b->pos = (uint64_t)(p % end);
    else
        stop_at_end(b);
}

/* Keeps the HIST frames before the new position: those the old copy still covers come from
   it (the game may have rewritten them since), the rest from the buffer. */
static void snapshot(Buf *b, const uint8_t *base, int merge)
{
    if (b->frames < HIST_MIN_FRAMES) {
        b->hist_ok = 0;
        return;
    }
    if (!b->hist && !(b->hist = malloc(2 * HIST * sizeof *b->hist)))
        fatal("out of memory");
    float tmp[2 * HIST];
    int64_t end = (int64_t)(b->pos >> 32);
    for (int k = 0; k < HIST; k++)
        frame_at(b, base, end - HIST + k, merge && b->hist_ok, &tmp[2 * k], &tmp[2 * k + 1]);
    memcpy(b->hist, tmp, sizeof tmp);
    b->hist_end = (uint32_t)end;
    b->hist_ok = 1;
}

static void mix_voice(Buf *b, float *out, int frames, int rate)
{
    float gl = b->gain_l * master_l, gr = b->gain_r * master_r;
    if (!out || (gl == 0 && gr == 0)) {
        advance(b, (uint64_t)frames, rate);
        return;
    }
    const uint8_t *base = (const uint8_t *)gptr(b->pcm->mem);
    uint64_t step = step_of(b, rate), end = (uint64_t)b->frames << 32, moved = 0;
    int half = kernel_half(step);
    /* the copy is needed until the kernel's back edge has passed the chunk's starting point */
    uint64_t hist_span = (uint64_t)half << 32, merge_span = (uint64_t)HIST << 32;
    int use_hist = b->hist_ok;
    for (int i = 0; i < frames; i++) {
        float l, r;
        interp(b, base, b->pos, step, half, use_hist && moved < hist_span, &l, &r);
        out[2 * i] += l * gl;
        out[2 * i + 1] += r * gr;
        b->pos += step;
        if (moved < merge_span)
            moved += step;
        if (b->pos >= end) {
            if (!b->looping) {
                stop_at_end(b);
                return;
            }
            b->pos %= end;
        }
    }
    snapshot(b, base, moved < merge_span);
}

/* The events of the offsets the play position passed from byte `from` (it moved `bytes` on;
   DSBPN_OFFSETSTOP's when it stopped) */
extern void event_signal(uint32_t h);
static void notify_passed(Buf *b, uint32_t from, uint64_t moved, int stopped)
{
    for (int i = 0; i < b->nnotify; i++) {
        uint32_t off = b->notify_off[i];
        int hit;
        if (off == 0xffffffffu)
            hit = stopped;
        else if (moved >= b->bytes)
            hit = 1;
        else {
            uint32_t d = off >= from ? off - from : off + b->bytes - from;    /* bytes ahead of from */
            hit = d < moved || (d == 0 && moved);
        }
        if (hit)
            event_signal(b->notify_ev[i]);
    }
}

static void step_voice(Buf *b, float *out, int frames, int rate)
{
    if (!b->nnotify) {
        mix_voice(b, out, frames, rate);
        return;
    }
    uint64_t p0 = b->pos;
    mix_voice(b, out, frames, rate);
    uint64_t total = (uint64_t)step_of(b, rate) * (uint64_t)frames;     /* 32.32 frames */
    uint32_t from = (uint32_t)(p0 >> 32) * b->block;
    uint64_t moved = (total >> 32) * b->block;
    if (!b->playing) {
        uint32_t left = b->bytes - from;
        notify_passed(b, from, moved < left ? moved : left, 1);
    } else
        notify_passed(b, from, moved, 0);
}

static void step_all(float *out, int frames, int rate)
{
    for (Buf *b = bufs; b; b = b->next)
        if (!b->primary && b->playing)
            step_voice(b, out, frames, rate);
    out_frames += (uint64_t)frames;
    out_rate = rate;
}

/* With nothing attached, catch every buffer up to the clock. Called under the lock. */
static void sync_clock(void)
{
    if (pull_internal || pull_external)
        return;
    double f = (clock_fn() - clock_anchor) * SOUND_CLOCK_RATE / 1000.0;
    if (f <= (double)clock_frames)
        return;
    uint64_t target = (uint64_t)f;
    while (clock_frames < target) {
        uint64_t n = target - clock_frames;
        if (n > 0x40000000u)
            n = 0x40000000u;
        step_all(NULL, (int)n, SOUND_CLOCK_RATE);
        clock_frames += n;
    }
}

static void reanchor_clock(void)
{
    clock_anchor = clock_fn();
    clock_frames = 0;
}

static double write_lead_ms(void)
{
    double chunk = pull_external ? chunk_ms_external : pull_internal ? chunk_ms_internal : 0;
    return chunk > SOUND_WRITE_LEAD_MS ? chunk : SOUND_WRITE_LEAD_MS;
}

/* Play and write cursors in bytes. The write cursor equals the play cursor while stopped; while
   playing it leads by the write lead plus what the interpolation kernel reads ahead. */
static void cursors(const Buf *b, uint32_t *play, uint32_t *write)
{
    uint32_t pf;
    if (b->primary)
        pf = (uint32_t)((out_frames * b->rate / (uint64_t)out_rate) % b->frames);
    else
        pf = (uint32_t)(b->pos >> 32);
    uint64_t r = (uint64_t)out_rate, fmax = b->freq > r ? b->freq : r;
    uint32_t ahead = (uint32_t)((KN * fmax + r - 1) / r) + 1;
    uint32_t lead = (uint32_t)ceil(b->freq * write_lead_ms() / 1000.0) + ahead;
    *play = pf * b->block;
    *write = b->playing ? (uint32_t)(((uint64_t)pf + lead) % b->frames) * b->block : *play;
}

/* ------------------------------------------------------------------ the mixer's public side */
/* internal: the WAV writer's own pull, which gives way to an attached device (returns 0) */
static int mix(float *out, int frames, int rate, int internal)
{
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    if (rate < 1000 || rate > 768000)
        return 1;
    pthread_mutex_lock(&snd_lock);
    if (internal && pull_external) {
        pthread_mutex_unlock(&snd_lock);
        return 0;
    }
    step_all(out, frames, rate);
    pthread_mutex_unlock(&snd_lock);
    for (int i = 0; i < 2 * frames; i++)
        out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
    wav_tap(out, frames, rate);
    return 1;
}

void sound_mix(float *out, int frames, int rate)
{
    if (frames > 0)
        mix(out, frames, rate, 0);
}

void sound_output_attach(int rate, int chunk_frames)
{
    pthread_mutex_lock(&snd_lock);
    sync_clock();
    pull_external = 1;
    chunk_ms_external = rate > 0 ? chunk_frames * 1000.0 / rate : 0;
    pthread_mutex_unlock(&snd_lock);
    LOG(LOG_SOUND, "sound output attached: %d Hz, chunks up to %d frames (write lead %.1f ms)",
        rate, chunk_frames, chunk_ms_external > SOUND_WRITE_LEAD_MS ? chunk_ms_external : SOUND_WRITE_LEAD_MS);
}

void sound_output_detach(void)
{
    pthread_mutex_lock(&snd_lock);
    pull_external = 0;
    if (!pull_internal)
        reanchor_clock();
    pthread_mutex_unlock(&snd_lock);
    LOG(LOG_SOUND, "sound output detached");
}

double sound_clock_ms(void)
{
    return clock_fn();
}

static int clock_gen;

void sound_set_clock(double (*fn)(void))
{
    pthread_mutex_lock(&snd_lock);
    sync_clock();
    clock_fn = fn ? fn : now_ms;
    reanchor_clock();
    clock_gen++;
    pthread_mutex_unlock(&snd_lock);
}

/* ------------------------------------------------------------------ objects */
static Buf *buf_new(DS *d)
{
    Buf *b = calloc(1, sizeof *b);
    if (!b)
        fatal("out of memory");
    b->ds = d;
    b->refs = 1;
    b->id = next_id++;
    b->obj = com_new(&buf_cls, b);
    b->next = bufs;
    bufs = b;
    b->gain3_l = b->gain3_r = 1.0f;
    b->doppler = 1.0f;
    static const float def3[16] = {0, 0, 0, 0, 0, 0, 0, 360, 360, 0, 0, 1, 0, 1, 1e9f, 0};
    memcpy(b->p3, def3, sizeof def3);
    memcpy(b->p3_pending, def3, sizeof def3);
    return b;
}

static void pcm_release(Pcm *p)
{
    if (p && --p->refs == 0) {
        gfree(p->mem);
        free(p);
    }
}

/* The program's pointer stays valid: it now reaches methods that only log and fail. */
static void buf_destroy(Buf *b)
{
    for (Buf **pp = &bufs; *pp; pp = &(*pp)->next)
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    if (b->primary && b->ds->primary == b)
        b->ds->primary = NULL;
    pcm_release(b->pcm);
    com_set_class(b->obj, &dead_buf_cls);
    free(b->hist);
    free(b);
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Reads a WAVEFORMATEX (p: program memory or a host copy); DS_OK if it is PCM we can play. */
static uint32_t parse_format(const uint8_t *p, Fmt *f)
{
    uint32_t tag = rd16(p), ch = rd16(p + 2), rate = rd32(p + 4), align = rd16(p + 12);
    uint32_t bits = rd16(p + 14), len = 18;
    if (tag == 0xfffe) {
        if (rd16(p + 16) < 22 || memcmp(p + 24, ksdataformat_pcm, 16))
            return DSERR_BADFORMAT;
        len = 40;
    } else if (tag != 1) {
        return DSERR_BADFORMAT;
    }
    if ((bits != 8 && bits != 16) || ch < 1 || ch > 2)
        return DSERR_BADFORMAT;
    if (rate < FREQ_MIN || rate > FREQ_MAX)
        return DSERR_INVALIDPARAM;
    if (align != ch * bits / 8)
        LOG(LOG_ALWAYS, "dsound: format block align %u for %u ch %u-bit; using %u", align, ch, bits,
            ch * bits / 8);
    memset(f, 0, sizeof *f);
    memcpy(f->raw, p, len);
    if (len == 18)
        f->raw[16] = f->raw[17] = 0;      /* cbSize means nothing for plain PCM */
    f->len = len;
    f->channels = ch;
    f->bits = bits;
    f->block = ch * bits / 8;
    f->rate = rate;
    return DS_OK;
}

static void apply_format(Buf *b, const Fmt *f)
{
    b->fmt = *f;
    b->channels = f->channels;
    b->bits = f->bits;
    b->block = f->block;
    b->rate = b->freq = f->rate;
}

static void primary_layout(Buf *b)
{
    b->frames = b->rate / 10;        /* a nominal 100 ms ring for the cursors */
    b->bytes = b->frames * b->block;
}

static DS *ds_new(int initialized)
{
    static const uint8_t default_primary[18] = {1, 0, 2, 0, 0x22, 0x56, 0, 0, 0x44, 0xac, 0, 0,
                                                2, 0, 8, 0, 0, 0};
    DS *d = calloc(1, sizeof *d);
    if (!d)
        fatal("out of memory");
    d->refs = 1;
    d->initialized = initialized;
    d->speaker = DSSPEAKER_STEREO_WIDE;
    memcpy(d->pfmt, default_primary, sizeof d->pfmt);
    d->obj = com_new(&ds_cls, d);
    static const float defl[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1};
    memcpy(d->ls, defl, sizeof defl);
    memcpy(d->ls_pending, defl, sizeof defl);
    return d;
}

static void ds_destroy(DS *d)
{
    for (Buf *b = bufs, *nx; b; b = nx) {
        nx = b->next;
        if (b->ds == d) {
            LOG(LOG_SOUND, "IDirectSound released: releasing buffer %s with it", BUFNAME(b));
            buf_destroy(b);
        }
    }
    com_set_class(d->obj, &dead_ds_cls);
    free(d);
}

/* ------------------------------------------------------------------ IDirectSound */
SMETHOD(ds_QueryInterface, 3)
{
    DS *d = com_host(ARG(0));
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[48];
    if (!ppv)
        FAIL(DSERR_INVALIDPARAM, "IDirectSound::QueryInterface(%s, NULL)", guid_str(riid, g, sizeof g));
    if (is_guid(riid, G_IUNKNOWN) || is_guid(riid, G_IDIRECTSOUND)) {
        d->refs++;
        W32(ppv, ARG(0));
        LOG(LOG_SOUND, "IDirectSound::QueryInterface(%s) -> self", guid_str(riid, g, sizeof g));
        return DS_OK;
    }
    W32(ppv, 0);
    LOG(LOG_ALWAYS, "IDirectSound::QueryInterface(%s): not implemented", guid_str(riid, g, sizeof g));
    return E_NOINTERFACE;
}

SMETHOD(ds_AddRef, 1)
{
    DS *d = com_host(ARG(0));
    LOG(LOG_SOUND, "IDirectSound::AddRef -> %d", d->refs + 1);
    return (uint32_t)++d->refs;
}

SMETHOD(ds_Release, 1)
{
    DS *d = com_host(ARG(0));
    int r = --d->refs;
    LOG(LOG_SOUND, "IDirectSound::Release -> %d", r);
    if (r <= 0) {
        ds_destroy(d);
        return 0;
    }
    return (uint32_t)r;
}

static uint32_t create_primary(DS *d, uint32_t flags, uint32_t pp)
{
    char f[256];
    if (d->primary) {
        d->primary->refs++;
        W32(pp, d->primary->obj);
        LOG(LOG_SOUND, "IDirectSound::CreateSoundBuffer(primary) -> existing %s %08x",
            BUFNAME(d->primary), d->primary->obj);
        return DS_OK;
    }
    if (flags & (DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLFX | DSBCAPS_CTRLPOSITIONNOTIFY)) {
        LOG(LOG_SOUND, "IDirectSound::CreateSoundBuffer(primary, %s): invalid flags",
            caps_str(flags, f, sizeof f));
        return DSERR_INVALIDPARAM;
    }
    Fmt fm;
    parse_format(d->pfmt, &fm);      /* always valid: Windows' default or one SetFormat took */
    Buf *b = buf_new(d);
    b->primary = 1;
    b->flags = flags;
    apply_format(b, &fm);
    primary_layout(b);
    update_gains(b);
    d->primary = b;
    W32(pp, b->obj);
    LOG(LOG_SOUND, "IDirectSound::CreateSoundBuffer(primary, %s) -> %s %08x",
        caps_str(flags, f, sizeof f), BUFNAME(b), b->obj);
    return DS_OK;
}

SMETHOD(ds_CreateSoundBuffer, 4)
{
    DS *d = com_host(ARG(0));
    uint32_t desc = ARG(1), pp = ARG(2), outer = ARG(3);
    char f[256], w[96];
    if (!d->initialized)
        FAIL(DSERR_UNINITIALIZED, "IDirectSound::CreateSoundBuffer: not initialized");
    if (!desc || !pp)
        FAIL(DSERR_INVALIDPARAM, "IDirectSound::CreateSoundBuffer(desc %08x, out %08x): NULL", desc, pp);
    W32(pp, 0);
    if (outer)
        FAIL(DSERR_NOAGGREGATION, "IDirectSound::CreateSoundBuffer: aggregation (outer %08x)", outer);
    uint32_t size = R32(desc), flags = R32(desc + 4), bytes = R32(desc + 8), wfx = R32(desc + 16);
    if (size != 20 && size != 36) {
        LOG(LOG_SOUND, "IDirectSound::CreateSoundBuffer: bad DSBUFFERDESC size %u", size);
        return DSERR_INVALIDPARAM;
    }
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (bytes || wfx) {
            LOG(LOG_SOUND, "IDirectSound::CreateSoundBuffer(primary): bytes %u / format %08x must be 0",
                bytes, wfx);
            return DSERR_INVALIDPARAM;
        }
        return create_primary(d, flags, pp);
    }
    if (!wfx || bytes < DSBSIZE_MIN || bytes > DSBSIZE_MAX) {
        LOG(LOG_SOUND, "IDirectSound::CreateSoundBuffer(%u bytes, format %08x): invalid", bytes, wfx);
        return DSERR_INVALIDPARAM;
    }
    Fmt fm;
    uint32_t hr = parse_format((const uint8_t *)gptr(wfx), &fm);
    if (hr) {
        LOG(LOG_ALWAYS, "IDirectSound::CreateSoundBuffer(%u bytes, %s): format not supported (%08x)",
            bytes, wfx_str(wfx, w, sizeof w), hr);
        return hr;
    }
    if (flags & DSBCAPS_CTRLFX)
        LOG(LOG_ALWAYS, "IDirectSound::CreateSoundBuffer: %s asked for; not implemented",
            caps_str(flags & DSBCAPS_CTRLFX, f, sizeof f));
    Buf *b = buf_new(d);
    apply_format(b, &fm);
    b->flags = flags;
    b->bytes = bytes;
    b->frames = bytes / b->block;
    b->pcm = calloc(1, sizeof *b->pcm);
    if (!b->pcm)
        fatal("out of memory");
    b->pcm->refs = 1;
    b->pcm->bytes = bytes;
    b->pcm->mem = galloc(bytes);
    if (b->bits == 8)
        memset(gptr(b->pcm->mem), 0x80, bytes);
    update_gains(b);
    W32(pp, b->obj);
    LOG(LOG_SOUND, "IDirectSound::CreateSoundBuffer(%u bytes, %s, %s) -> %s %08x (memory %08x)",
        bytes, wfx_str(wfx, w, sizeof w), caps_str(flags, f, sizeof f), BUFNAME(b), b->obj, b->pcm->mem);
    return DS_OK;
}

SMETHOD(ds_GetCaps, 2)
{
    DS *d = com_host(ARG(0));
    uint32_t p = ARG(1);
    if (!d->initialized)
        FAIL(DSERR_UNINITIALIZED, "IDirectSound::GetCaps: not initialized");
    if (!p || R32(p) != 96) {
        LOG(LOG_SOUND, "IDirectSound::GetCaps: bad DSCAPS (size %u)", p ? R32(p) : 0);
        return DSERR_INVALIDPARAM;
    }
    memset(gptr(p + 4), 0, 92);
    /* a software device like current Windows': every PCM format, no hardware mixing */
    W32(p + 4, 0xf5f);           /* PRIMARY{MONO,STEREO,8BIT,16BIT} CONTINUOUSRATE CERTIFIED SECONDARY* */
    W32(p + 8, FREQ_MIN);
    W32(p + 12, FREQ_MAX);
    W32(p + 16, 1);              /* dwPrimaryBuffers */
    LOG(LOG_SOUND, "IDirectSound::GetCaps");
    return DS_OK;
}

SMETHOD(ds_DuplicateSoundBuffer, 3)
{
    DS *d = com_host(ARG(0));
    uint32_t orig = ARG(1), pp = ARG(2);
    if (!d->initialized)
        FAIL(DSERR_UNINITIALIZED, "IDirectSound::DuplicateSoundBuffer: not initialized");
    if (!orig || !pp)
        FAIL(DSERR_INVALIDPARAM, "IDirectSound::DuplicateSoundBuffer(%08x, %08x): NULL", orig, pp);
    W32(pp, 0);
    if (R32(orig) != buf_cls.vtbl) {
        LOG(LOG_ALWAYS, "IDirectSound::DuplicateSoundBuffer(%08x): not a live sound buffer", orig);
        return DSERR_INVALIDPARAM;
    }
    Buf *o = com_host(orig);
    if (o->primary) {
        LOG(LOG_SOUND, "IDirectSound::DuplicateSoundBuffer(%s): primary", BUFNAME(o));
        return DSERR_INVALIDCALL;
    }
    Buf *b = buf_new(d);           /* same format, memory, frequency, volume and pan; stopped at 0 */
    apply_format(b, &o->fmt);
    b->flags = o->flags;
    b->bytes = o->bytes;
    b->frames = o->frames;
    b->pcm = o->pcm;
    b->pcm->refs++;
    b->freq = o->freq;
    b->volume = o->volume;
    b->pan = o->pan;
    update_gains(b);
    W32(pp, b->obj);
    LOG(LOG_SOUND, "IDirectSound::DuplicateSoundBuffer(%s) -> %s %08x", BUFNAME(o), BUFNAME(b), b->obj);
    return DS_OK;
}

SMETHOD(ds_SetCooperativeLevel, 3)
{
    DS *d = com_host(ARG(0));
    uint32_t hwnd = ARG(1), level = ARG(2);
    static const char *names[] = {"?", "NORMAL", "PRIORITY", "EXCLUSIVE", "WRITEPRIMARY"};
    if (!d->initialized)
        FAIL(DSERR_UNINITIALIZED, "IDirectSound::SetCooperativeLevel: not initialized");
    if (level < 1 || level > 4) {
        LOG(LOG_SOUND, "IDirectSound::SetCooperativeLevel(hwnd %08x, %u): invalid level", hwnd, level);
        return DSERR_INVALIDPARAM;
    }
    if (level == DSSCL_WRITEPRIMARY)
        LOG(LOG_ALWAYS, "IDirectSound::SetCooperativeLevel: WRITEPRIMARY (writing the primary "
            "buffer) is not implemented");
    d->hwnd = hwnd;
    d->level = level;
    LOG(LOG_SOUND, "IDirectSound::SetCooperativeLevel(hwnd %08x, %s)", hwnd, names[level]);
    return DS_OK;
}

SMETHOD(ds_Compact, 1)
{
    DS *d = com_host(ARG(0));
    if (!d->initialized)
        FAIL(DSERR_UNINITIALIZED, "IDirectSound::Compact: not initialized");
    LOG(LOG_SOUND, "IDirectSound::Compact");
    return d->level >= DSSCL_PRIORITY ? DS_OK : DSERR_PRIOLEVELNEEDED;
}

SMETHOD(ds_GetSpeakerConfig, 2)
{
    DS *d = com_host(ARG(0));
    if (!d->initialized)
        FAIL(DSERR_UNINITIALIZED, "IDirectSound::GetSpeakerConfig: not initialized");
    if (!ARG(1))
        FAIL(DSERR_INVALIDPARAM, "IDirectSound::GetSpeakerConfig(NULL)");
    W32(ARG(1), d->speaker);
    LOG(LOG_SOUND, "IDirectSound::GetSpeakerConfig -> %08x", d->speaker);
    return DS_OK;
}

SMETHOD(ds_SetSpeakerConfig, 2)
{
    DS *d = com_host(ARG(0));
    if (!d->initialized)
        FAIL(DSERR_UNINITIALIZED, "IDirectSound::SetSpeakerConfig: not initialized");
    d->speaker = ARG(1);
    LOG(LOG_SOUND, "IDirectSound::SetSpeakerConfig(%08x)", ARG(1));
    return DS_OK;
}

SMETHOD(ds_Initialize, 2)
{
    DS *d = com_host(ARG(0));
    char g[48];
    LOG(LOG_SOUND, "IDirectSound::Initialize(device %s)%s", guid_str(ARG(1), g, sizeof g),
        d->initialized ? ": already initialized" : "");
    if (d->initialized)
        return DSERR_ALREADYINITIALIZED;
    d->initialized = 1;
    return DS_OK;
}

/* ------------------------------------------------------------------ IDirectSoundBuffer */
SMETHOD(buf_QueryInterface, 3)
{
    Buf *b = com_host(ARG(0));
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[48];
    if (!ppv)
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::QueryInterface(%s, %s, NULL)", BUFNAME(b),
             guid_str(riid, g, sizeof g));
    if (is_guid(riid, G_IUNKNOWN) || is_guid(riid, G_IDIRECTSOUNDBUFFER)) {
        b->refs++;
        W32(ppv, ARG(0));
        LOG(LOG_SOUND, "IDirectSoundBuffer::QueryInterface(%s, %s) -> self", BUFNAME(b),
            guid_str(riid, g, sizeof g));
        return DS_OK;
    }
    static const uint8_t notify_d4[8] = {0xaf, 0x08, 0x00, 0xa0, 0xc9, 0x25, 0xcd, 0x16};
    static const uint8_t ds3d_d4[8] = {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60};
    if (guid_eq(riid, 0x279afa84u, 0x4981, 0x11ce, ds3d_d4) && b->primary && (b->flags & DSBCAPS_CTRL3D)) {
        DS *d = b->ds;
        if (!d->listener)
            d->listener = com_new(&ls_cls, d);
        b->refs++;
        W32(ppv, d->listener);
        LOG(LOG_SOUND, "IDirectSoundBuffer::QueryInterface(%s, IDirectSound3DListener) -> %08x", BUFNAME(b), d->listener);
        return DS_OK;
    }
    if (guid_eq(riid, 0x279afa86u, 0x4981, 0x11ce, ds3d_d4) && !b->primary && (b->flags & DSBCAPS_CTRL3D)) {
        if (!b->obj3d)
            b->obj3d = com_new(&b3_cls, b);
        b->refs++;
        W32(ppv, b->obj3d);
        place_3d(b);
        return DS_OK;
    }
    if (guid_eq(riid, 0xb0210783u, 0x89cd, 0x11d0, notify_d4) && !b->primary) {
        if (!b->notify_obj)
            b->notify_obj = com_new(&notify_cls, b);
        b->refs++;
        W32(ppv, b->notify_obj);
        LOG(LOG_SOUND, "IDirectSoundBuffer::QueryInterface(%s, IDirectSoundNotify) -> %08x", BUFNAME(b), b->notify_obj);
        return DS_OK;
    }
    W32(ppv, 0);
    LOG(LOG_ALWAYS, "IDirectSoundBuffer::QueryInterface(%s, %s): not implemented", BUFNAME(b),
        guid_str(riid, g, sizeof g));
    return E_NOINTERFACE;
}

SMETHOD(buf_AddRef, 1)
{
    Buf *b = com_host(ARG(0));
    LOG(LOG_SOUND, "IDirectSoundBuffer::AddRef(%s) -> %d", BUFNAME(b), b->refs + 1);
    return (uint32_t)++b->refs;
}

SMETHOD(buf_Release, 1)
{
    Buf *b = com_host(ARG(0));
    int r = --b->refs;
    LOG(LOG_SOUND, "IDirectSoundBuffer::Release(%s) -> %d", BUFNAME(b), r);
    if (r <= 0) {
        buf_destroy(b);
        return 0;
    }
    return (uint32_t)r;
}

SMETHOD(buf_GetCaps, 2)
{
    Buf *b = com_host(ARG(0));
    uint32_t p = ARG(1);
    if (!p || R32(p) != 20) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::GetCaps(%s): bad DSBCAPS (size %u)", BUFNAME(b), p ? R32(p) : 0);
        return DSERR_INVALIDPARAM;
    }
    uint32_t fl = b->flags;
    if (!(fl & (DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE)))
        fl |= DSBCAPS_LOCSOFTWARE;
    W32(p + 4, fl);
    W32(p + 8, b->bytes);
    W32(p + 12, 0);
    W32(p + 16, 0);
    LOG(LOG_SOUND, "IDirectSoundBuffer::GetCaps(%s) -> flags %08x, %u bytes", BUFNAME(b), fl, b->bytes);
    return DS_OK;
}

SMETHOD(buf_GetCurrentPosition, 3)
{
    Buf *b = com_host(ARG(0));
    uint32_t pplay = ARG(1), pwrite = ARG(2), play, write;
    sync_clock();
    cursors(b, &play, &write);
    if (pplay)
        W32(pplay, play);
    if (pwrite)
        W32(pwrite, write);
    VLOG("IDirectSoundBuffer::GetCurrentPosition(%s) -> play %u, write %u", BUFNAME(b), play, write);
    return DS_OK;
}

SMETHOD(buf_GetFormat, 4)
{
    Buf *b = com_host(ARG(0));
    uint32_t wfx = ARG(1), size = ARG(2), pwritten = ARG(3);
    if (!wfx && !pwritten)
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::GetFormat(%s, NULL, NULL)", BUFNAME(b));
    if (wfx) {
        if (size < b->fmt.len) {
            LOG(LOG_SOUND, "IDirectSoundBuffer::GetFormat(%s): %u bytes is too small", BUFNAME(b), size);
            return DSERR_INVALIDPARAM;
        }
        memcpy(gptr(wfx), b->fmt.raw, b->fmt.len);
    }
    if (pwritten)
        W32(pwritten, b->fmt.len);
    LOG(LOG_SOUND, "IDirectSoundBuffer::GetFormat(%s) -> %u ch, %u Hz, %u-bit", BUFNAME(b),
        b->channels, b->rate, b->bits);
    return DS_OK;
}

SMETHOD(buf_GetVolume, 2)
{
    Buf *b = com_host(ARG(0));
    if (!ARG(1))
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::GetVolume(%s, NULL)", BUFNAME(b));
    if (!(b->flags & DSBCAPS_CTRLVOLUME))
        FAIL(DSERR_CONTROLUNAVAIL, "IDirectSoundBuffer::GetVolume(%s): no CTRLVOLUME", BUFNAME(b));
    W32(ARG(1), (uint32_t)b->volume);
    VLOG("IDirectSoundBuffer::GetVolume(%s) -> %d", BUFNAME(b), b->volume);
    return DS_OK;
}

SMETHOD(buf_GetPan, 2)
{
    Buf *b = com_host(ARG(0));
    if (!ARG(1))
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::GetPan(%s, NULL)", BUFNAME(b));
    if (!(b->flags & DSBCAPS_CTRLPAN))
        FAIL(DSERR_CONTROLUNAVAIL, "IDirectSoundBuffer::GetPan(%s): no CTRLPAN", BUFNAME(b));
    W32(ARG(1), (uint32_t)b->pan);
    VLOG("IDirectSoundBuffer::GetPan(%s) -> %d", BUFNAME(b), b->pan);
    return DS_OK;
}

SMETHOD(buf_GetFrequency, 2)
{
    Buf *b = com_host(ARG(0));
    if (!ARG(1))
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::GetFrequency(%s, NULL)", BUFNAME(b));
    if (b->primary || !(b->flags & DSBCAPS_CTRLFREQUENCY))
        FAIL(DSERR_CONTROLUNAVAIL, "IDirectSoundBuffer::GetFrequency(%s): no CTRLFREQUENCY", BUFNAME(b));
    W32(ARG(1), b->freq);
    VLOG("IDirectSoundBuffer::GetFrequency(%s) -> %u", BUFNAME(b), b->freq);
    return DS_OK;
}

SMETHOD(buf_GetStatus, 2)
{
    Buf *b = com_host(ARG(0));
    if (!ARG(1))
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::GetStatus(%s, NULL)", BUFNAME(b));
    sync_clock();
    uint32_t st = 0;
    if (b->playing) {
        st = DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0);
        if (b->flags & DSBCAPS_LOCDEFER)
            st |= DSBSTATUS_LOCSOFTWARE;
    }
    W32(ARG(1), st);
    VLOG("IDirectSoundBuffer::GetStatus(%s) -> %x", BUFNAME(b), st);
    return DS_OK;
}

SMETHOD(buf_Initialize, 3)
{
    Buf *b = com_host(ARG(0));
    LOG(LOG_SOUND, "IDirectSoundBuffer::Initialize(%s): already initialized", BUFNAME(b));
    return DSERR_ALREADYINITIALIZED;
}

SMETHOD(buf_Lock, 8)
{
    Buf *b = com_host(ARG(0));
    uint32_t off = ARG(1), n = ARG(2), pp1 = ARG(3), pn1 = ARG(4), pp2 = ARG(5), pn2 = ARG(6);
    uint32_t fl = ARG(7);
    if (b->primary) {
        if (b->ds->level == DSSCL_WRITEPRIMARY) {
            LOG(LOG_ALWAYS, "IDirectSoundBuffer::Lock(%s): writing the primary buffer is not implemented",
                BUFNAME(b));
            return DSERR_UNSUPPORTED;
        }
        LOG(LOG_SOUND, "IDirectSoundBuffer::Lock(%s): needs WRITEPRIMARY", BUFNAME(b));
        return DSERR_PRIOLEVELNEEDED;
    }
    if (!pp1 || !pn1)
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::Lock(%s, offset %u, %u bytes): NULL out pointer",
             BUFNAME(b), off, n);
    if (fl & DSBLOCK_FROMWRITECURSOR) {
        uint32_t play;
        sync_clock();
        cursors(b, &play, &off);
    }
    if (fl & DSBLOCK_ENTIREBUFFER)
        n = b->bytes;
    if (off >= b->bytes || n > b->bytes || !n) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::Lock(%s, offset %u, %u bytes, flags %x): out of range "
            "(buffer %u bytes)", BUFNAME(b), off, n, fl, b->bytes);
        return DSERR_INVALIDPARAM;
    }
    uint32_t n1 = n < b->bytes - off ? n : b->bytes - off, n2 = pp2 ? n - n1 : 0;
    W32(pp1, b->pcm->mem + off);
    W32(pn1, n1);
    if (pp2)
        W32(pp2, n2 ? b->pcm->mem : 0);
    if (pn2)
        W32(pn2, n2);
    VLOG("IDirectSoundBuffer::Lock(%s, offset %u, %u bytes, flags %x) -> %08x+%u, %08x+%u", BUFNAME(b),
         off, n, fl, b->pcm->mem + off, n1, n2 ? b->pcm->mem : 0, n2);
    return DS_OK;
}

SMETHOD(buf_Play, 4)
{
    Buf *b = com_host(ARG(0));
    uint32_t prio = ARG(2), fl = ARG(3);
    if (prio && !(b->flags & DSBCAPS_LOCDEFER)) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::Play(%s, priority %u): priority needs LOCDEFER", BUFNAME(b), prio);
        return DSERR_INVALIDPARAM;
    }
    if (b->primary) {
        if (!(fl & DSBPLAY_LOOPING)) {
            LOG(LOG_SOUND, "IDirectSoundBuffer::Play(%s, flags %x): primary must loop", BUFNAME(b), fl);
            return DSERR_INVALIDPARAM;
        }
        b->playing = b->looping = 1;
        LOG(LOG_SOUND, "IDirectSoundBuffer::Play(%s, LOOPING)", BUFNAME(b));
        return DS_OK;
    }
    sync_clock();
    if (!b->playing)
        b->hist_ok = 0;             /* the game may have rewritten everything while stopped */
    b->playing = b->frames > 0;
    b->looping = (fl & DSBPLAY_LOOPING) != 0;
    LOG(LOG_SOUND, "IDirectSoundBuffer::Play(%s, %s) at byte %u, %u Hz, vol %d, pan %d", BUFNAME(b),
        b->looping ? "LOOPING" : "once", (uint32_t)(b->pos >> 32) * b->block, b->freq, b->volume, b->pan);
    return DS_OK;
}

SMETHOD(buf_SetCurrentPosition, 2)
{
    Buf *b = com_host(ARG(0));
    uint32_t pos = ARG(1);
    if (b->primary)
        FAIL(DSERR_INVALIDCALL, "IDirectSoundBuffer::SetCurrentPosition(%s, %u): primary", BUFNAME(b), pos);
    if (pos >= b->bytes) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetCurrentPosition(%s, %u): beyond %u bytes", BUFNAME(b), pos,
            b->bytes);
        return DSERR_INVALIDPARAM;
    }
    sync_clock();
    b->pos = (uint64_t)(pos / b->block) << 32;
    b->hist_ok = 0;
    LOG(LOG_SOUND, "IDirectSoundBuffer::SetCurrentPosition(%s, %u)", BUFNAME(b), pos);
    return DS_OK;
}

SMETHOD(buf_SetFormat, 2)
{
    Buf *b = com_host(ARG(0));
    uint32_t wfx = ARG(1);
    char w[96];
    if (!b->primary) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetFormat(%s): only the primary buffer has a settable format",
            BUFNAME(b));
        return DSERR_INVALIDCALL;
    }
    if (!wfx)
        FAIL(DSERR_INVALIDPARAM, "IDirectSoundBuffer::SetFormat(%s, NULL)", BUFNAME(b));
    if (b->ds->level < DSSCL_PRIORITY) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetFormat(%s, %s): needs PRIORITY", BUFNAME(b),
            wfx_str(wfx, w, sizeof w));
        return DSERR_PRIOLEVELNEEDED;
    }
    Fmt fm;
    uint32_t hr = parse_format((const uint8_t *)gptr(wfx), &fm);
    if (hr) {
        LOG(LOG_ALWAYS, "IDirectSoundBuffer::SetFormat(%s, %s): format not supported (%08x)", BUFNAME(b),
            wfx_str(wfx, w, sizeof w), hr);
        return hr;
    }
    fm.raw[0] = 1;                 /* the primary keeps a plain WAVEFORMATEX */
    fm.raw[1] = 0;
    fm.raw[16] = fm.raw[17] = 0;
    fm.len = 18;
    apply_format(b, &fm);
    primary_layout(b);
    memcpy(b->ds->pfmt, fm.raw, 18);
    /* the device keeps mixing at its own rate, as on current Windows */
    LOG(LOG_SOUND, "IDirectSoundBuffer::SetFormat(%s, %s)", BUFNAME(b), wfx_str(wfx, w, sizeof w));
    return DS_OK;
}

SMETHOD(buf_SetVolume, 2)
{
    Buf *b = com_host(ARG(0));
    int32_t v = (int32_t)ARG(1);
    if (!(b->flags & DSBCAPS_CTRLVOLUME)) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetVolume(%s, %d): no CTRLVOLUME", BUFNAME(b), v);
        return DSERR_CONTROLUNAVAIL;
    }
    if (v < VOL_MIN || v > 0) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetVolume(%s, %d): out of range", BUFNAME(b), v);
        return DSERR_INVALIDPARAM;
    }
    sync_clock();
    b->volume = v;
    update_gains(b);
    VLOG("IDirectSoundBuffer::SetVolume(%s, %d)", BUFNAME(b), v);
    return DS_OK;
}

SMETHOD(buf_SetPan, 2)
{
    Buf *b = com_host(ARG(0));
    int32_t v = (int32_t)ARG(1);
    if (!(b->flags & DSBCAPS_CTRLPAN)) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetPan(%s, %d): no CTRLPAN", BUFNAME(b), v);
        return DSERR_CONTROLUNAVAIL;
    }
    if (v < -10000 || v > 10000) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetPan(%s, %d): out of range", BUFNAME(b), v);
        return DSERR_INVALIDPARAM;
    }
    sync_clock();
    b->pan = v;
    update_gains(b);
    VLOG("IDirectSoundBuffer::SetPan(%s, %d)", BUFNAME(b), v);
    return DS_OK;
}

SMETHOD(buf_SetFrequency, 2)
{
    Buf *b = com_host(ARG(0));
    uint32_t f = ARG(1);
    if (b->primary || !(b->flags & DSBCAPS_CTRLFREQUENCY)) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetFrequency(%s, %u): no CTRLFREQUENCY", BUFNAME(b), f);
        return DSERR_CONTROLUNAVAIL;
    }
    if (f && (f < FREQ_MIN || f > FREQ_MAX)) {
        LOG(LOG_SOUND, "IDirectSoundBuffer::SetFrequency(%s, %u): out of range", BUFNAME(b), f);
        return DSERR_INVALIDPARAM;
    }
    sync_clock();                  /* the time so far was played at the old frequency */
    b->freq = f ? f : b->rate;
    VLOG("IDirectSoundBuffer::SetFrequency(%s, %u)", BUFNAME(b), b->freq);
    return DS_OK;
}

SMETHOD(buf_Stop, 1)
{
    Buf *b = com_host(ARG(0));
    sync_clock();
    LOG(LOG_SOUND, "IDirectSoundBuffer::Stop(%s)%s at byte %u", BUFNAME(b), b->playing ? "" : " (not playing)",
        b->primary ? 0 : (uint32_t)(b->pos >> 32) * b->block);
    int was = b->playing;
    b->playing = 0;
    if (was && b->nnotify)
        notify_passed(b, 0, 0, 1);      /* DSBPN_OFFSETSTOP */
    return DS_OK;
}

SMETHOD(buf_Unlock, 5)
{
    Buf *b = com_host(ARG(0));
    uint32_t p1 = ARG(1), n1 = ARG(2), p2 = ARG(3), n2 = ARG(4);
    if (b->primary)
        FAIL(DSERR_INVALIDCALL, "IDirectSoundBuffer::Unlock(%s): primary", BUFNAME(b));
    uint32_t mem = b->pcm->mem;
    if ((p1 && (p1 < mem || p1 + n1 > mem + b->bytes)) || (p2 && (p2 < mem || p2 + n2 > mem + b->bytes))) {
        LOG(LOG_ALWAYS, "IDirectSoundBuffer::Unlock(%s, %08x+%u, %08x+%u): not inside the buffer %08x+%u",
            BUFNAME(b), p1, n1, p2, n2, mem, b->bytes);
        return DSERR_INVALIDPARAM;
    }
    VLOG("IDirectSoundBuffer::Unlock(%s, %08x+%u, %08x+%u)", BUFNAME(b), p1, n1, p2, n2);
    return DS_OK;
}

SMETHOD(buf_Restore, 1)
{
    Buf *b = com_host(ARG(0));
    LOG(LOG_SOUND, "IDirectSoundBuffer::Restore(%s)", BUFNAME(b));
    return DS_OK;                  /* buffers are never lost */
}

/* ------------------------------------------------------------------ released objects */
static int dead_logged;
static uint32_t dead_call(CPU *c, int nargs)
{
    if (__atomic_fetch_add(&dead_logged, 1, __ATOMIC_RELAXED) < 20)
        LOG(LOG_ALWAYS, "dsound: call with %d args on released object %08x (return address %08x)", nargs,
            ARG(0), R32(c->esp));
    return nargs == 1 ? 0 : DSERR_INVALIDCALL;   /* AddRef/Release/Stop/Restore/Compact */
}
METHOD(dead1, 1) { return dead_call(c, 1); }
METHOD(dead2, 2) { return dead_call(c, 2); }
METHOD(dead3, 3) { return dead_call(c, 3); }
METHOD(dead4, 4) { return dead_call(c, 4); }
METHOD(dead5, 5) { return dead_call(c, 5); }
METHOD(dead8, 8) { return dead_call(c, 8); }

/* ------------------------------------------------------------------ vtables */
static void (*const ds_methods[])(CPU *) = {
    ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps,
    ds_DuplicateSoundBuffer, ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig,
    ds_SetSpeakerConfig, ds_Initialize};
static const char *const ds_names[] = {
    "QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
    "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig",
    "SetSpeakerConfig", "Initialize"};
static void (*const dead_ds_methods[])(CPU *) = {
    dead3, dead1, dead1, dead4, dead2, dead3, dead3, dead1, dead2, dead2, dead2};

static void (*const buf_methods[])(CPU *) = {
    buf_QueryInterface, buf_AddRef, buf_Release, buf_GetCaps, buf_GetCurrentPosition,
    buf_GetFormat, buf_GetVolume, buf_GetPan, buf_GetFrequency, buf_GetStatus, buf_Initialize,
    buf_Lock, buf_Play, buf_SetCurrentPosition, buf_SetFormat, buf_SetVolume, buf_SetPan,
    buf_SetFrequency, buf_Stop, buf_Unlock, buf_Restore};
static const char *const buf_names[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition",
    "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize",
    "Lock", "Play", "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan",
    "SetFrequency", "Stop", "Unlock", "Restore"};
static void (*const dead_buf_methods[])(CPU *) = {
    dead3, dead1, dead1, dead2, dead3, dead4, dead2, dead2, dead2, dead2, dead3,
    dead8, dead4, dead2, dead2, dead2, dead2, dead2, dead1, dead5, dead1};
static const char *const dead_names[] = {
    "dead", "dead", "dead", "dead", "dead", "dead", "dead", "dead", "dead", "dead", "dead",
    "dead", "dead", "dead", "dead", "dead", "dead", "dead", "dead", "dead", "dead"};

#define COUNT(a) ((int)(sizeof a / sizeof *a))
_Static_assert(COUNT(ds_methods) == 11 && COUNT(ds_names) == 11 && COUNT(dead_ds_methods) == 11,
               "IDirectSound has 11 methods");
_Static_assert(COUNT(buf_methods) == 21 && COUNT(buf_names) == 21 && COUNT(dead_buf_methods) == 21,
               "IDirectSoundBuffer has 21 methods");

/* ------------------------------------------------------------------ creation */
/* DirectSoundCreate(lpGuid, ppDS, pUnkOuter), imported by ordinal */
API(DSOUND, 1, 3)
{
    uint32_t guid = ARG(0), pp = ARG(1), outer = ARG(2);
    char g[48];
    LOG(LOG_SOUND, "DirectSoundCreate(device %s, %08x, outer %08x)", guid_str(guid, g, sizeof g), pp, outer);
    if (!pp)
        FAIL(DSERR_INVALIDPARAM, "DirectSoundCreate: NULL out pointer");
    W32(pp, 0);
    if (outer)
        FAIL(DSERR_NOAGGREGATION, "DirectSoundCreate: aggregation not supported");
    pthread_mutex_lock(&snd_lock);
    DS *d = ds_new(1);
    pthread_mutex_unlock(&snd_lock);
    W32(pp, d->obj);
    LOG(LOG_SOUND, "DirectSoundCreate -> %08x", d->obj);
    return DS_OK;
}

/* DirectSoundEnumerateA (ordinal 2): one device, the primary sound driver */
API(DSOUND, 2, 2)
{
    uint32_t cb = ARG(0), ctx = ARG(1);
    static uint32_t desc, module;
    if (!desc) {
        desc = gstrdup("Primary Sound Driver");
        module = gstrdup("");
    }
    LOG(LOG_SOUND, "DirectSoundEnumerateA(cb %08x)", cb);
    if (cb)
        guest_call(c, cb, 4, 0, desc, module, ctx);
    return DS_OK;
}


/* ------------------------------------------------------------------ 3D sound
   DirectSound 7's software 3D model for CTRL3D buffers: the listener (position, velocity, front
   and top vectors, distance factor in metres per unit, rolloff, Doppler factor) and each buffer's
   position, velocity, minimum and maximum distance and mode (NORMAL, HEADRELATIVE, DISABLE).
   Distance: full volume within the minimum distance, then (min / d) ^ rolloff, no further fall
   past the maximum; direction: panned by the source's side in the listener's frame (left-handed:
   right = top x front), the far ear down to about -9 dB at the side; Doppler: the frequency times
   (c + listener's speed toward the source) / (c - source's speed toward the listener), c = 343 m/s.
   Cones (none in TD6) are not modelled. Settings made with DS3D_DEFERRED wait for
   CommitDeferredSettings. */
#define SPEED_OF_SOUND 343.0f

static void place_3d(Buf *b)
{
    DS *d = b->ds;
    const float *L = d->ls, *P = b->p3;
    uint32_t mode;
    memcpy(&mode, &P[15], 4);
    if (mode == 2) {                                    /* DS3DMODE_DISABLE */
        b->gain3_l = b->gain3_r = 1.0f;
        b->doppler = 1.0f;
        update_gains(b);
        return;
    }
    float rel[3];
    for (int i = 0; i < 3; i++)
        rel[i] = mode == 1 ? P[1 + i] : P[1 + i] - L[1 + i];
    float dist = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    float mind = P[13] > 0 ? P[13] : 1.0f, maxd = P[14] > mind ? P[14] : mind;
    float dd = dist < mind ? mind : dist > maxd ? maxd : dist;
    float g = powf(mind / dd, L[14] > 0 ? L[14] : 0.0f);
    /* the side: in the listener's frame unless head-relative */
    float side = 0;
    if (dist > 1e-6f) {
        float right[3];
        if (mode == 1) {
            right[0] = 1; right[1] = 0; right[2] = 0;
        } else {
            const float *f = &L[7], *t = &L[10];
            right[0] = t[1] * f[2] - t[2] * f[1];
            right[1] = t[2] * f[0] - t[0] * f[2];
            right[2] = t[0] * f[1] - t[1] * f[0];
            float n = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
            if (n > 1e-6f)
                for (int i = 0; i < 3; i++) right[i] /= n;
        }
        side = (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / dist;
    }
    b->gain3_l = g * (side > 0 ? 1.0f - 0.65f * side : 1.0f);
    b->gain3_r = g * (side < 0 ? 1.0f + 0.65f * side : 1.0f);
    /* Doppler, in metres per second */
    float df = L[13] > 0 ? L[13] : 1.0f, dop = 1.0f;
    if (L[15] > 0 && dist > 1e-6f) {
        float u[3] = {rel[0] / dist, rel[1] / dist, rel[2] / dist};    /* listener -> source */
        float vl = 0, vs = 0;
        for (int i = 0; i < 3; i++) {
            vl += (mode == 1 ? 0 : L[4 + i]) * u[i];
            vs += P[4 + i] * u[i];
        }
        vl *= df * L[15];
        vs *= df * L[15];
        float c = SPEED_OF_SOUND;
        if (vs < -0.9f * c) vs = -0.9f * c;
        if (vs > 0.9f * c) vs = 0.9f * c;
        dop = (c + vl) / (c + vs);
        if (dop < 0.25f) dop = 0.25f;
        if (dop > 4.0f) dop = 4.0f;
    }
    b->doppler = dop;
    update_gains(b);
}

static void place_all_3d(DS *d)
{
    for (Buf *b = bufs; b; b = b->next)
        if (b->ds == d && !b->primary && (b->flags & DSBCAPS_CTRL3D))
            place_3d(b);
}

static void rdv(uint32_t a, float *v, int n) { for (int i = 0; i < n; i++) { uint32_t u = R32(a + 4 * i); memcpy(&v[i], &u, 4); } }
static void wrv(uint32_t a, const float *v, int n) { for (int i = 0; i < n; i++) { uint32_t u; memcpy(&u, &v[i], 4); W32(a + 4 * i, u); } }

/* Settings land in the pending copy; DS3D_IMMEDIATE (0) applies them at once */
static void commit_3d(DS *d)
{
    sync_clock();                       /* what has played so far, at the old placement */
    if (d->ls_dirty) {
        memcpy(d->ls, d->ls_pending, sizeof d->ls);
        d->ls_dirty = 0;
    }
    for (Buf *b = bufs; b; b = b->next)
        if (b->ds == d && b->p3_dirty) {
            memcpy(b->p3, b->p3_pending, sizeof b->p3);
            b->p3_dirty = 0;
        }
    place_all_3d(d);
}

static void ls_set(DS *d, int at, const float *v, int n, uint32_t apply)
{
    memcpy(&d->ls_pending[at], v, (size_t)n * 4);
    d->ls_dirty = 1;
    if (apply == 0)
        commit_3d(d);
}

static void b3_set(Buf *b, int at, const float *v, int n, uint32_t apply)
{
    memcpy(&b->p3_pending[at], v, (size_t)n * 4);
    b->p3_dirty = 1;
    if (apply == 0)
        commit_3d(b->ds);
}

/* IDirectSound3DListener: host = the DS (the listener belongs to the primary buffer) */
SMETHOD(ls_QueryInterface, 3)
{
    if (ARG(2)) W32(ARG(2), 0);
    return E_NOINTERFACE;
}
SMETHOD(ls_AddRef, 1) { DS *d = com_host(ARG(0)); return d->primary ? (uint32_t)++d->primary->refs : 1; }
SMETHOD(ls_Release, 1)
{
    DS *d = com_host(ARG(0));
    if (!d->primary) return 0;
    int r = --d->primary->refs;
    if (r <= 0) {
        buf_destroy(d->primary);
        return 0;
    }
    return (uint32_t)r;
}
SMETHOD(ls_GetAllParameters, 2)
{
    DS *d = com_host(ARG(0));
    if (!ARG(1) || R32(ARG(1)) < 64) return DSERR_INVALIDPARAM;
    wrv(ARG(1) + 4, &d->ls[1], 15);
    return DS_OK;
}
SMETHOD(ls_GetDistanceFactor, 2) { DS *d = com_host(ARG(0)); wrv(ARG(1), &d->ls[13], 1); return DS_OK; }
SMETHOD(ls_GetDopplerFactor, 2) { DS *d = com_host(ARG(0)); wrv(ARG(1), &d->ls[15], 1); return DS_OK; }
SMETHOD(ls_GetOrientation, 3) { DS *d = com_host(ARG(0)); wrv(ARG(1), &d->ls[7], 3); wrv(ARG(2), &d->ls[10], 3); return DS_OK; }
SMETHOD(ls_GetPosition, 2) { DS *d = com_host(ARG(0)); wrv(ARG(1), &d->ls[1], 3); return DS_OK; }
SMETHOD(ls_GetRolloffFactor, 2) { DS *d = com_host(ARG(0)); wrv(ARG(1), &d->ls[14], 1); return DS_OK; }
SMETHOD(ls_GetVelocity, 2) { DS *d = com_host(ARG(0)); wrv(ARG(1), &d->ls[4], 3); return DS_OK; }
SMETHOD(ls_SetAllParameters, 3)
{
    DS *d = com_host(ARG(0));
    if (!ARG(1) || R32(ARG(1)) < 64) return DSERR_INVALIDPARAM;
    float v[15];
    rdv(ARG(1) + 4, v, 15);
    ls_set(d, 1, v, 15, ARG(2));
    return DS_OK;
}
#define LS_SET1(name, at) SMETHOD(ls_Set##name, 3) { DS *d = com_host(ARG(0)); float v[1]; rdv(c->esp + 8, v, 1); ls_set(d, at, v, 1, ARG(2)); return DS_OK; }
#define LS_SET3(name, at) SMETHOD(ls_Set##name, 5) { DS *d = com_host(ARG(0)); float v[3]; rdv(c->esp + 8, v, 3); ls_set(d, at, v, 3, ARG(4)); return DS_OK; }
LS_SET1(DistanceFactor, 13)
LS_SET1(DopplerFactor, 15)
LS_SET1(RolloffFactor, 14)
LS_SET3(Position, 1)
LS_SET3(Velocity, 4)
SMETHOD(ls_SetOrientation, 8)
{
    DS *d = com_host(ARG(0));
    float v[6];
    rdv(c->esp + 8, v, 6);
    ls_set(d, 7, v, 6, ARG(7));
    return DS_OK;
}
SMETHOD(ls_CommitDeferredSettings, 1)
{
    commit_3d(com_host(ARG(0)));
    return DS_OK;
}
static void (*const ls_methods[])(CPU *) = {ls_QueryInterface, ls_AddRef, ls_Release, ls_GetAllParameters,
    ls_GetDistanceFactor, ls_GetDopplerFactor, ls_GetOrientation, ls_GetPosition, ls_GetRolloffFactor,
    ls_GetVelocity, ls_SetAllParameters, ls_SetDistanceFactor, ls_SetDopplerFactor, ls_SetOrientation,
    ls_SetPosition, ls_SetRolloffFactor, ls_SetVelocity, ls_CommitDeferredSettings};
static const char *const ls_names[] = {"QueryInterface", "AddRef", "Release", "GetAllParameters",
    "GetDistanceFactor", "GetDopplerFactor", "GetOrientation", "GetPosition", "GetRolloffFactor", "GetVelocity",
    "SetAllParameters", "SetDistanceFactor", "SetDopplerFactor", "SetOrientation", "SetPosition",
    "SetRolloffFactor", "SetVelocity", "CommitDeferredSettings"};

/* IDirectSound3DBuffer: host = the Buf, sharing its reference count. DS3DBUFFER: dwSize, position,
   velocity, inside and outside cone angles, cone orientation, cone outside volume, min, max, mode */
SMETHOD(b3_QueryInterface, 3)
{
    Buf *b = com_host(ARG(0));
    if (ARG(2)) W32(ARG(2), 0);
    if (ARG(2) && (is_guid(ARG(1), G_IDIRECTSOUNDBUFFER) || is_guid(ARG(1), G_IUNKNOWN))) {
        b->refs++;
        W32(ARG(2), b->obj);
        return DS_OK;
    }
    return E_NOINTERFACE;
}
SMETHOD(b3_AddRef, 1) { Buf *b = com_host(ARG(0)); return (uint32_t)++b->refs; }
SMETHOD(b3_Release, 1)
{
    Buf *b = com_host(ARG(0));
    int r = --b->refs;
    if (r <= 0) {
        buf_destroy(b);
        return 0;
    }
    return (uint32_t)r;
}
static void p3_to_prog(const float *p, uint32_t a)
{
    wrv(a + 4, &p[1], 6);
    uint32_t u;
    W32(a + 0x1c, (uint32_t)p[7]);
    W32(a + 0x20, (uint32_t)p[8]);
    wrv(a + 0x24, &p[9], 3);
    W32(a + 0x30, (uint32_t)(int32_t)p[12]);
    wrv(a + 0x34, &p[13], 2);
    memcpy(&u, &p[15], 4);
    W32(a + 0x3c, u);
}
SMETHOD(b3_GetAllParameters, 2)
{
    Buf *b = com_host(ARG(0));
    if (!ARG(1) || R32(ARG(1)) < 64) return DSERR_INVALIDPARAM;
    p3_to_prog(b->p3, ARG(1));
    return DS_OK;
}
SMETHOD(b3_GetConeAngles, 3) { Buf *b = com_host(ARG(0)); if (ARG(1)) W32(ARG(1), (uint32_t)b->p3[7]); if (ARG(2)) W32(ARG(2), (uint32_t)b->p3[8]); return DS_OK; }
SMETHOD(b3_GetConeOrientation, 2) { Buf *b = com_host(ARG(0)); wrv(ARG(1), &b->p3[9], 3); return DS_OK; }
SMETHOD(b3_GetConeOutsideVolume, 2) { Buf *b = com_host(ARG(0)); W32(ARG(1), (uint32_t)(int32_t)b->p3[12]); return DS_OK; }
SMETHOD(b3_GetMaxDistance, 2) { Buf *b = com_host(ARG(0)); wrv(ARG(1), &b->p3[14], 1); return DS_OK; }
SMETHOD(b3_GetMinDistance, 2) { Buf *b = com_host(ARG(0)); wrv(ARG(1), &b->p3[13], 1); return DS_OK; }
SMETHOD(b3_GetMode, 2) { Buf *b = com_host(ARG(0)); uint32_t u; memcpy(&u, &b->p3[15], 4); W32(ARG(1), u); return DS_OK; }
SMETHOD(b3_GetPosition, 2) { Buf *b = com_host(ARG(0)); wrv(ARG(1), &b->p3[1], 3); return DS_OK; }
SMETHOD(b3_GetVelocity, 2) { Buf *b = com_host(ARG(0)); wrv(ARG(1), &b->p3[4], 3); return DS_OK; }
SMETHOD(b3_SetAllParameters, 3)
{
    Buf *b = com_host(ARG(0));
    uint32_t a = ARG(1);
    if (!a || R32(a) < 64) return DSERR_INVALIDPARAM;
    float v[15];
    rdv(a + 4, &v[0], 6);
    v[6] = (float)R32(a + 0x1c);
    v[7] = (float)R32(a + 0x20);
    rdv(a + 0x24, &v[8], 3);
    v[11] = (float)(int32_t)R32(a + 0x30);
    rdv(a + 0x34, &v[12], 2);
    uint32_t mode = R32(a + 0x3c);
    memcpy(&v[14], &mode, 4);
    b3_set(b, 1, v, 15, ARG(2));
    return DS_OK;
}
SMETHOD(b3_SetConeAngles, 4) { Buf *b = com_host(ARG(0)); float v[2] = {(float)ARG(1), (float)ARG(2)}; b3_set(b, 7, v, 2, ARG(3)); return DS_OK; }
SMETHOD(b3_SetConeOrientation, 5) { Buf *b = com_host(ARG(0)); float v[3]; rdv(c->esp + 8, v, 3); b3_set(b, 9, v, 3, ARG(4)); return DS_OK; }
SMETHOD(b3_SetConeOutsideVolume, 3) { Buf *b = com_host(ARG(0)); float v = (float)(int32_t)ARG(1); b3_set(b, 12, &v, 1, ARG(2)); return DS_OK; }
SMETHOD(b3_SetMaxDistance, 3) { Buf *b = com_host(ARG(0)); float v; rdv(c->esp + 8, &v, 1); b3_set(b, 14, &v, 1, ARG(2)); return DS_OK; }
SMETHOD(b3_SetMinDistance, 3) { Buf *b = com_host(ARG(0)); float v; rdv(c->esp + 8, &v, 1); b3_set(b, 13, &v, 1, ARG(2)); return DS_OK; }
SMETHOD(b3_SetMode, 3) { Buf *b = com_host(ARG(0)); uint32_t m = ARG(1); float v; memcpy(&v, &m, 4); b3_set(b, 15, &v, 1, ARG(2)); return DS_OK; }
SMETHOD(b3_SetPosition, 5) { Buf *b = com_host(ARG(0)); float v[3]; rdv(c->esp + 8, v, 3); b3_set(b, 1, v, 3, ARG(4)); return DS_OK; }
SMETHOD(b3_SetVelocity, 5) { Buf *b = com_host(ARG(0)); float v[3]; rdv(c->esp + 8, v, 3); b3_set(b, 4, v, 3, ARG(4)); return DS_OK; }
static void (*const b3_methods[])(CPU *) = {b3_QueryInterface, b3_AddRef, b3_Release, b3_GetAllParameters,
    b3_GetConeAngles, b3_GetConeOrientation, b3_GetConeOutsideVolume, b3_GetMaxDistance, b3_GetMinDistance,
    b3_GetMode, b3_GetPosition, b3_GetVelocity, b3_SetAllParameters, b3_SetConeAngles, b3_SetConeOrientation,
    b3_SetConeOutsideVolume, b3_SetMaxDistance, b3_SetMinDistance, b3_SetMode, b3_SetPosition, b3_SetVelocity};
static const char *const b3_names[] = {"QueryInterface", "AddRef", "Release", "GetAllParameters",
    "GetConeAngles", "GetConeOrientation", "GetConeOutsideVolume", "GetMaxDistance", "GetMinDistance", "GetMode",
    "GetPosition", "GetVelocity", "SetAllParameters", "SetConeAngles", "SetConeOrientation",
    "SetConeOutsideVolume", "SetMaxDistance", "SetMinDistance", "SetMode", "SetPosition", "SetVelocity"};

/* IDirectSoundNotify: one per buffer, sharing its reference count */
SMETHOD(nt_QueryInterface, 3)
{
    Buf *b = com_host(ARG(0));
    if (ARG(2)) W32(ARG(2), 0);
    if (!ARG(2)) return DSERR_INVALIDPARAM;
    if (is_guid(ARG(1), G_IDIRECTSOUNDBUFFER) || is_guid(ARG(1), G_IUNKNOWN)) {
        b->refs++;
        W32(ARG(2), b->obj);
        return DS_OK;
    }
    return E_NOINTERFACE;
}
SMETHOD(nt_AddRef, 1)
{
    Buf *b = com_host(ARG(0));
    return (uint32_t)++b->refs;
}
SMETHOD(nt_Release, 1)
{
    Buf *b = com_host(ARG(0));
    int r = --b->refs;
    if (r <= 0) {
        buf_destroy(b);
        return 0;
    }
    return (uint32_t)r;
}
/* (count, DSBPOSITIONNOTIFY[] {offset, event}) */
SMETHOD(nt_SetNotificationPositions, 3)
{
    Buf *b = com_host(ARG(0));
    uint32_t n = ARG(1), a = ARG(2);
    if (b->playing)
        return DSERR_INVALIDCALL;
    if (n > 32 || (n && !a))
        return DSERR_INVALIDPARAM;
    for (uint32_t i = 0; i < n; i++) {
        b->notify_off[i] = R32(a + 8 * i);
        b->notify_ev[i] = R32(a + 8 * i + 4);
        if (b->notify_off[i] != 0xffffffffu && b->notify_off[i] >= b->bytes)
            return DSERR_INVALIDPARAM;
    }
    b->nnotify = (int)n;
    LOG(LOG_SOUND, "IDirectSoundNotify::SetNotificationPositions(%s, %u)", BUFNAME(b), n);
    return DS_OK;
}
static void (*const notify_methods[])(CPU *) = {nt_QueryInterface, nt_AddRef, nt_Release,
                                                 nt_SetNotificationPositions};
static const char *const notify_names[] = {"QueryInterface", "AddRef", "Release", "SetNotificationPositions"};

uint32_t dsound_cocreate(uint32_t clsid, uint32_t riid, uint32_t ppv)
{
    char g[48];
    if (!is_guid(clsid, G_CLSID_DIRECTSOUND))
        return REGDB_E_CLASSNOTREG;
    if (!ppv)
        FAIL(DSERR_INVALIDPARAM, "CoCreateInstance(CLSID_DirectSound, %s, NULL)", guid_str(riid, g, sizeof g));
    W32(ppv, 0);
    if (!is_guid(riid, G_IUNKNOWN) && !is_guid(riid, G_IDIRECTSOUND)) {
        LOG(LOG_ALWAYS, "CoCreateInstance(CLSID_DirectSound, %s): interface not implemented",
            guid_str(riid, g, sizeof g));
        return E_NOINTERFACE;
    }
    pthread_mutex_lock(&snd_lock);
    DS *d = ds_new(0);
    pthread_mutex_unlock(&snd_lock);
    W32(ppv, d->obj);
    LOG(LOG_SOUND, "CoCreateInstance(CLSID_DirectSound, %s) -> %08x", guid_str(riid, g, sizeof g), d->obj);
    return DS_OK;
}

/* ------------------------------------------------------------------ WAV writer (debug aid) */
static FILE *wav_fp;
static int wav_on;                 /* read without the lock by every sound_mix */
static pthread_mutex_t wav_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t wav_frames;
static int wav_rate, wav_rate_warned;

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void wav_header(void)       /* under wav_lock */
{
    uint64_t d64 = wav_frames * 4;
    uint32_t data = d64 > 0xffffffffu - 36 ? 0xffffffffu - 36 : (uint32_t)d64;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put32(h + 20, 1 | 2u << 16);              /* PCM, 2 channels */
    put32(h + 24, (uint32_t)wav_rate);
    put32(h + 28, (uint32_t)wav_rate * 4);
    put32(h + 32, 4 | 16u << 16);             /* block align 4, 16 bits */
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    fseek(wav_fp, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, wav_fp);
    fseek(wav_fp, 0, SEEK_END);
}

static void wav_tap(const float *out, int frames, int rate)
{
    if (!__atomic_load_n(&wav_on, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&wav_lock);
    if (wav_fp) {
        if (rate != wav_rate && !wav_rate_warned) {
            wav_rate_warned = 1;
            LOG(LOG_ALWAYS, "sound WAV: output rate %d Hz differs from the file's %d Hz", rate, wav_rate);
        }
        int16_t pcm[1024];
        for (int i = 0; i < 2 * frames;) {
            int n = 2 * frames - i < 1024 ? 2 * frames - i : 1024;
            for (int k = 0; k < n; k++)
                pcm[k] = (int16_t)lrintf(out[i + k] * 32767.0f);   /* little-endian host */
            fwrite(pcm, 2, (size_t)n, wav_fp);
            i += n;
        }
        wav_frames += (uint64_t)frames;
    }
    pthread_mutex_unlock(&wav_lock);
}

static void wav_close(void)
{
    pthread_mutex_lock(&wav_lock);
    if (wav_fp) {
        wav_header();
        fclose(wav_fp);
        wav_fp = NULL;
        __atomic_store_n(&wav_on, 0, __ATOMIC_RELEASE);
        LOG(LOG_ALWAYS, "sound WAV: %.1f s written", (double)wav_frames / wav_rate);
    }
    pthread_mutex_unlock(&wav_lock);
}

/* Stands in for an audio device: pulls sound_mix in real time while no device is attached. */
static void *wav_thread(void *arg)
{
    (void)arg;
    enum { RATE = 44100, CHUNK = 256 };
    float buf[2 * CHUNK];
    double t0 = 0;
    uint64_t done = 0, flushed = 0;
    int running = 0, gen = -1;
    for (;;) {
        pthread_mutex_lock(&snd_lock);
        int ext = pull_external;
        if (ext) {
            running = 0;           /* the device's mix reaches the file through the tap */
            pull_internal = 0;
        } else if (!running || gen != clock_gen) {
            sync_clock();
            pull_internal = 1;
            chunk_ms_internal = CHUNK * 1000.0 / RATE;
            t0 = clock_fn();
            gen = clock_gen;
            done = 0;
            running = 1;
        }
        pthread_mutex_unlock(&snd_lock);
        if (running) {
            uint64_t target = (uint64_t)((sound_clock_ms() - t0) * RATE / 1000.0);
            while (running && done < target) {
                int n = target - done < CHUNK ? (int)(target - done) : CHUNK;
                running = mix(buf, n, RATE, 1);
                done += (uint64_t)n;
            }
        }
        pthread_mutex_lock(&wav_lock);
        if (wav_fp && wav_frames - flushed >= RATE) {      /* keep the file playable if killed */
            wav_header();
            fflush(wav_fp);
            flushed = wav_frames;
        }
        pthread_mutex_unlock(&wav_lock);
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void wav_start(const char *path)
{
    wav_fp = fopen(path, "wb");
    if (!wav_fp) {
        LOG(LOG_ALWAYS, "sound WAV: cannot write %s", path);
        return;
    }
    wav_rate = 44100;
    wav_header();
    __atomic_store_n(&wav_on, 1, __ATOMIC_RELEASE);
    atexit(wav_close);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, wav_thread, NULL)) {
        LOG(LOG_ALWAYS, "sound WAV: cannot start the writer thread");
        __atomic_store_n(&wav_on, 0, __ATOMIC_RELEASE);
        fclose(wav_fp);
        wav_fp = NULL;
    } else {
        LOG(LOG_ALWAYS, "sound WAV: recording the mix to %s", path);
    }
    pthread_attr_destroy(&at);
}

/* ------------------------------------------------------------------ init */
void dsound_init(void)
{
    const char *v = getenv("TD6_LOG_SOUND_VERBOSE");
    verbose = v && *v && strcmp(v, "0");
    build_kernel();
    com_class_init(&ds_cls, "IDirectSound", 11, ds_methods, ds_names);
    com_class_init(&buf_cls, "IDirectSoundBuffer", 21, buf_methods, buf_names);
    com_class_init(&notify_cls, "IDirectSoundNotify", 4, notify_methods, notify_names);
    com_class_init(&ls_cls, "IDirectSound3DListener", 18, ls_methods, ls_names);
    com_class_init(&b3_cls, "IDirectSound3DBuffer", 21, b3_methods, b3_names);
    com_class_init(&dead_ds_cls, "IDirectSound(released)", 11, dead_ds_methods, dead_names);
    com_class_init(&dead_buf_cls, "IDirectSoundBuffer(released)", 21, dead_buf_methods, dead_names);
    pthread_mutex_lock(&snd_lock);
    reanchor_clock();
    pthread_mutex_unlock(&snd_lock);
    const char *wav = getenv("TD6_SOUND_WAV");
    if (wav && *wav)
        wav_start(wav);
}

/* ------------------------------------------------------------------ the runtime's own voices
   Sound the runtime plays itself (the intro videos' soundtrack): an internal buffer the program
   never sees, mixed like the others. */
int sound_stream_start(const int16_t *pcm, uint32_t frames, int channels, int rate)
{
    uint8_t wfx[18] = {0};
    uint32_t block = 2u * (uint32_t)channels;
    wfx[0] = 1;
    wfx[2] = (uint8_t)channels;
    memcpy(wfx + 4, &rate, 4);
    uint32_t bps = (uint32_t)rate * block;
    memcpy(wfx + 8, &bps, 4);
    wfx[12] = (uint8_t)block;
    wfx[14] = 16;
    Fmt fm;
    if (parse_format(wfx, &fm) || !frames)
        return -1;
    Buf *b = calloc(1, sizeof *b);
    if (!b)
        fatal("out of memory");
    pthread_mutex_lock(&snd_lock);
    sync_clock();
    b->refs = 1;
    b->id = next_id++;
    apply_format(b, &fm);
    b->bytes = frames * block;
    b->frames = frames;
    b->pcm = calloc(1, sizeof *b->pcm);
    b->pcm->refs = 1;
    b->pcm->bytes = b->bytes;
    b->pcm->mem = galloc(b->bytes);
    memcpy(gptr(b->pcm->mem), pcm, b->bytes);
    update_gains(b);
    b->playing = 1;
    b->next = bufs;
    bufs = b;
    pthread_mutex_unlock(&snd_lock);
    LOG(LOG_SOUND, "runtime voice #%d: %u frames, %d ch, %d Hz", b->id, frames, channels, rate);
    return b->id;
}

void sound_stream_stop(int id)
{
    pthread_mutex_lock(&snd_lock);
    sync_clock();
    for (Buf **pp = &bufs; *pp; pp = &(*pp)->next) {
        Buf *b = *pp;
        if (b->id == id && !b->obj) {
            *pp = b->next;
            pcm_release(b->pcm);
            free(b->hist);
            free(b);
            break;
        }
    }
    pthread_mutex_unlock(&snd_lock);
}
