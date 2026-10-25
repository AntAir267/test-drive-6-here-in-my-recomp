/* WINMM: the millisecond clock, multimedia timers (a host thread per timer calling the
   program's callback), buffered RIFF file reading (mmio*) and MCI (videos: not yet). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"

extern void set_current_cpu(CPU *c);

/* ------------------------------------------------------------------ time */
API(WINMM, timeGetTime, 0) { return (uint32_t)(int64_t)now_ms(); }
API(WINMM, timeBeginPeriod, 1) { return 0; }
API(WINMM, timeEndPeriod, 1) { return 0; }

/* ------------------------------------------------------------------ timers */
typedef struct {
    uint32_t id, delay, callback, user, periodic;
    double due;                          /* --test: next virtual time to fire */
    volatile int stop;
    pthread_t pt;
    CPU *cpu;
} Timer;
#define MAXT 16
static Timer *timers[MAXT];
static pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER;

static void *timer_main(void *arg)
{
    Timer *t = arg;
    set_current_cpu(t->cpu);
    fpu_activate(t->cpu);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += (long)t->delay * 1000000L;
        while (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
        if (t->stop)
            break;
        guest_call(t->cpu, t->callback, 5, t->id, 0, t->user, 0, 0);
        if (!t->periodic)
            break;
    }
    return NULL;
}

API(WINMM, timeSetEvent, 5)
{
    pthread_mutex_lock(&tlock);
    int slot = -1;
    for (int i = 0; i < MAXT; i++)
        if (!timers[i]) { slot = i; break; }
    if (slot < 0) {
        pthread_mutex_unlock(&tlock);
        return 0;
    }
    Timer *t = calloc(1, sizeof *t);
    t->id = slot + 1;
    t->delay = ARG(0) ? ARG(0) : 1;
    t->callback = ARG(2);
    t->user = ARG(3);
    t->periodic = ARG(4) & 1;
    t->cpu = cpu_new(0x40000);
    t->due = now_ms() + t->delay;
    timers[slot] = t;
    pthread_mutex_unlock(&tlock);
    if (ARG(4) & ~1u)
        LOG(LOG_ALWAYS, "timeSetEvent: flags %x not supported (callback only)", ARG(4));
    LOG(LOG_THREAD, "timeSetEvent(%u ms, res %u, %08x, %x, %s) -> %u (thread %d)", ARG(0), ARG(1),
        t->callback, t->user, t->periodic ? "periodic" : "once", t->id, t->cpu->thread_index);
    if (!deterministic)
        pthread_create(&t->pt, NULL, timer_main, t);
    return t->id;
}

/* --test: timers fire on the calling (main) thread at frame ends, in id order. */
void timers_run_due(double now)
{
    CPU *self = current_cpu();
    for (int i = 0; i < MAXT; i++) {
        Timer *t = timers[i];
        while (t && t->due <= now && !t->stop) {
            t->due += t->delay;
            set_current_cpu(t->cpu);
            fpu_activate(t->cpu);           /* the callback's thread has its own x87 state */
            guest_call(t->cpu, t->callback, 5, t->id, 0, t->user, 0, 0);
            set_current_cpu(self);
            if (self)
                fpu_activate(self);
            if (!t->periodic) {
                timers[i] = NULL;
                break;
            }
            t = timers[i];
        }
    }
}

API(WINMM, timeKillEvent, 1)
{
    uint32_t id = ARG(0);
    pthread_mutex_lock(&tlock);
    Timer *t = id >= 1 && id <= MAXT ? timers[id - 1] : NULL;
    if (t) timers[id - 1] = NULL;
    pthread_mutex_unlock(&tlock);
    if (!t) return 97;                  /* MMSYSERR_INVALPARAM */
    t->stop = 1;
    if (!deterministic && !pthread_equal(t->pt, pthread_self()))
        pthread_join(t->pt, NULL);
    LOG(LOG_THREAD, "timeKillEvent(%u)", id);
    return 0;
}

/* ------------------------------------------------------------------ mmio
   MMIOINFO (72 bytes): dwFlags 0, fccIOProc 4, pIOProc 8, wErrorRet 12, htask 16, cchBuffer 20,
   pchBuffer 24, pchNext 28, pchEndRead 32, pchEndWrite 36, lBufOffset 40, lDiskOffset 44,
   adwInfo 48, dwReserved1 60, dwReserved2 64, hmmio 68.
   MMCKINFO (20 bytes): ckid 0, cksize 4, fccType 8, dwDataOffset 12, dwFlags 16. */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FOURCC_RIFF FOURCC('R', 'I', 'F', 'F')
#define FOURCC_LIST FOURCC('L', 'I', 'S', 'T')
#define FOURCC_MEM FOURCC('M', 'E', 'M', ' ')

typedef struct {
    int fd;                  /* -1 for memory files */
    uint32_t buf, cap;       /* program-memory buffer */
    uint32_t next, end;      /* current read pointer, end of valid data (program addresses) */
    int64_t buf_off;         /* file offset of buf */
    int64_t size;            /* file size */
    int own_buf;
} Mmio;

#define MAXM 32
static Mmio *mmios[MAXM];

static Mmio *mmio_of(uint32_t h) { return h >= 1 && h <= MAXM ? mmios[h - 1] : NULL; }

static int64_t mm_pos(Mmio *m) { return m->buf_off + (m->next - m->buf); }

static void mm_fill(Mmio *m, int64_t pos)
{
    if (m->fd < 0) {
        if (pos < 0) pos = 0;
        if (pos > m->size) pos = m->size;
        m->next = m->buf + (uint32_t)pos;
        return;
    }
    m->buf_off = pos;
    ssize_t r = pread(m->fd, gptr(m->buf), m->cap, pos);
    if (r < 0) r = 0;
    m->next = m->buf;
    m->end = m->buf + (uint32_t)r;
}

static int64_t mm_seek(Mmio *m, int64_t pos)
{
    if (pos < 0) return -1;
    if (m->fd < 0) {
        mm_fill(m, pos);
        return pos;
    }
    if (pos >= m->buf_off && pos <= m->buf_off + (m->end - m->buf))
        m->next = m->buf + (uint32_t)(pos - m->buf_off);
    else
        mm_fill(m, pos);
    return pos;
}

static uint32_t mm_read(Mmio *m, uint32_t dst, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        if (m->next >= m->end) {
            if (m->fd < 0) break;
            mm_fill(m, mm_pos(m));
            if (m->next >= m->end) break;
        }
        uint32_t k = m->end - m->next;
        if (k > n - got) k = n - got;
        memcpy(gptr(dst + got), gptr(m->next), k);
        m->next += k;
        got += k;
    }
    return got;
}

API(WINMM, mmioOpenA, 3)
{
    uint32_t info = ARG(1), flags = ARG(2);
    if (flags & 3) {
        LOG(LOG_ALWAYS, "mmioOpenA: writing not supported (flags %x)", flags);
        return 0;
    }
    Mmio *m = calloc(1, sizeof *m);
    if (info && R32(info + 4) == FOURCC_MEM) {
        m->fd = -1;
        m->buf = R32(info + 24);
        m->cap = R32(info + 20);
        m->size = m->cap;
        m->next = m->buf;
        m->end = m->buf + m->cap;
    } else {
        char host[2048];
        if (!path_to_host(gstr(ARG(0)), host, sizeof host, 0) || (m->fd = open(host, O_RDONLY | O_CLOEXEC)) < 0) {
            LOG(LOG_FILE, "mmioOpenA(\"%s\") -> not found", gstr(ARG(0)));
            free(m);
            if (info) W32(info + 12, 257);   /* MMIOERR_FILENOTFOUND */
            return 0;
        }
        m->size = lseek(m->fd, 0, SEEK_END);
        m->cap = (info && R32(info + 20)) ? R32(info + 20) : 8192;
        m->buf = galloc(m->cap);
        m->own_buf = 1;
        mm_fill(m, 0);
    }
    int slot = -1;
    for (int i = 0; i < MAXM; i++)
        if (!mmios[i]) { slot = i; break; }
    if (slot < 0) fatal("too many open mmio files");
    mmios[slot] = m;
    LOG(LOG_FILE, "mmioOpenA(\"%s\", flags %x) -> %d (%lld bytes)", m->fd < 0 ? "<memory>" : gstr(ARG(0)),
        flags, slot + 1, (long long)m->size);
    return slot + 1;
}

API(WINMM, mmioClose, 2)
{
    Mmio *m = mmio_of(ARG(0));
    if (!m) return 5;
    if (m->fd >= 0) close(m->fd);
    if (m->own_buf) gfree(m->buf);
    mmios[ARG(0) - 1] = NULL;
    free(m);
    return 0;
}

API(WINMM, mmioRead, 3)
{
    Mmio *m = mmio_of(ARG(0));
    if (!m) return 0xffffffffu;
    return mm_read(m, ARG(1), ARG(2));
}

API(WINMM, mmioSeek, 3)
{
    Mmio *m = mmio_of(ARG(0));
    if (!m) return 0xffffffffu;
    int64_t off = (int32_t)ARG(1);
    int64_t base = ARG(2) == 1 ? mm_pos(m) : ARG(2) == 2 ? m->size : 0;
    return (uint32_t)mm_seek(m, base + off);
}

API(WINMM, mmioGetInfo, 3)
{
    Mmio *m = mmio_of(ARG(0));
    if (!m) return 5;
    uint32_t i = ARG(1);
    memset(gptr(i), 0, 72);
    W32(i + 0, 0x10000);                           /* MMIO_ALLOCBUF / read */
    W32(i + 4, m->fd < 0 ? FOURCC_MEM : FOURCC('D', 'O', 'S', ' '));
    W32(i + 20, m->cap);
    W32(i + 24, m->buf);
    W32(i + 28, m->next);
    W32(i + 32, m->end);
    W32(i + 36, m->buf);
    W32(i + 40, (uint32_t)m->buf_off);
    W32(i + 44, (uint32_t)(m->buf_off + (m->end - m->buf)));
    W32(i + 68, ARG(0));
    return 0;
}

API(WINMM, mmioSetInfo, 3)
{
    Mmio *m = mmio_of(ARG(0));
    if (!m) return 5;
    uint32_t nx = R32(ARG(1) + 28);
    if (nx >= m->buf && nx <= m->end)
        m->next = nx;
    return 0;
}

API(WINMM, mmioAdvance, 3)
{
    Mmio *m = mmio_of(ARG(0));
    if (!m) return 5;
    uint32_t i = ARG(1);
    if (i) {
        uint32_t nx = R32(i + 28);
        if (nx >= m->buf && nx <= m->end) m->next = nx;
    }
    if (m->fd >= 0)
        mm_fill(m, mm_pos(m));
    if (i) {
        W32(i + 24, m->buf);
        W32(i + 28, m->next);
        W32(i + 32, m->end);
        W32(i + 40, (uint32_t)m->buf_off);
        W32(i + 44, (uint32_t)(m->buf_off + (m->end - m->buf)));
    }
    return 0;
}

API(WINMM, mmioDescend, 4)
{
    Mmio *m = mmio_of(ARG(0));
    uint32_t ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!m) return 5;
    uint32_t want = 0, want_type = 0;
    if (flags & 0x10) want = R32(ck);                        /* MMIO_FINDCHUNK */
    if (flags & 0x20) { want = FOURCC_RIFF; want_type = R32(ck + 8); }   /* MMIO_FINDRIFF */
    if (flags & 0x40) { want = FOURCC_LIST; want_type = R32(ck + 8); }   /* MMIO_FINDLIST */
    int64_t limit = parent ? (int64_t)R32(parent + 12) + R32(parent + 4) : m->size;
    for (;;) {
        int64_t at = mm_pos(m);
        if (at + 8 > limit) return 0x0101;                   /* MMIOERR_CHUNKNOTFOUND */
        uint32_t hdr = galloc(12);
        uint32_t got = mm_read(m, hdr, 8);
        uint32_t id = R32(hdr), size = R32(hdr + 4), type = 0;
        if (got < 8) { gfree(hdr); return 0x0101; }
        int has_type = id == FOURCC_RIFF || id == FOURCC_LIST;
        if (has_type) {
            mm_read(m, hdr + 8, 4);
            type = R32(hdr + 8);
        }
        gfree(hdr);
        int match = !want || (id == want && (!want_type || type == want_type));
        if (match) {
            W32(ck, id);
            W32(ck + 4, size);
            W32(ck + 8, type);
            W32(ck + 12, (uint32_t)(at + 8));
            W32(ck + 16, 0);
            if (has_type) {
                /* data offset points after the form type */
            }
            return 0;
        }
        mm_seek(m, at + 8 + size + (size & 1));
    }
}

API(WINMM, mmioAscend, 3)
{
    Mmio *m = mmio_of(ARG(0));
    if (!m) return 5;
    uint32_t ck = ARG(1);
    uint32_t size = R32(ck + 4);
    mm_seek(m, (int64_t)R32(ck + 12) + size + (size & 1));
    return 0;
}

/* ------------------------------------------------------------------ MCI */
API(WINMM, mciSendCommandA, 4)
{
    LOG(LOG_API, "mciSendCommandA(dev %u, msg %x, flags %x): no CD audio", ARG(0), ARG(1), ARG(2));
    return 263;                          /* MCIERR_INVALID_DEVICE_NAME: TD6 plays its WMA music instead */
}

API(WINMM, mciGetErrorStringA, 3)
{
    if (ARG(2)) gstrncpy(ARG(1), "The specified device is not open or is not recognized by MCI.", ARG(2));
    return 1;
}

/* Auxiliary audio (the CD's volume): none */
API(WINMM, auxGetNumDevs, 0) { return 0; }
API(WINMM, auxGetDevCapsA, 3) { return 2; }        /* MMSYSERR_BADDEVICEID */
API(WINMM, auxSetVolume, 2) { return 2; }
