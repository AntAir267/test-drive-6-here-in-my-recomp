/* Kernel objects: the handle table, threads, events, mutexes, critical sections and waits.
   Waits use one process-wide lock and condition variable: simple, and these objects are
   few and rarely contended. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include "runtime.h"

extern void set_current_cpu(CPU *c);

static pthread_mutex_t klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kcond = PTHREAD_COND_INITIALIZER;

/* ------------------------------------------------------------------ handles */
#define MAXH 4096
#define HBASE 0x1000u
static struct { int type; void *obj; } htab[MAXH];

uint32_t handle_new(int type, void *obj)
{
    pthread_mutex_lock(&klock);
    for (int i = 1; i < MAXH; i++)
        if (htab[i].type == H_FREE) {
            htab[i].type = type;
            htab[i].obj = obj;
            pthread_mutex_unlock(&klock);
            return HBASE + 4u * i;
        }
    pthread_mutex_unlock(&klock);
    fatal("out of handles");
}

static int hindex(uint32_t h)
{
    if (h < HBASE || (h & 3))
        return -1;
    uint32_t i = (h - HBASE) / 4;
    return i < MAXH ? (int)i : -1;
}

void *handle_get(uint32_t h, int type)
{
    int i = hindex(h);
    if (i < 0 || htab[i].type != type)
        return NULL;
    return htab[i].obj;
}

int handle_type(uint32_t h)
{
    int i = hindex(h);
    return i < 0 ? H_FREE : htab[i].type;
}

void handle_close(uint32_t h)
{
    int i = hindex(h);
    if (i < 0)
        return;
    pthread_mutex_lock(&klock);
    htab[i].type = H_FREE;
    htab[i].obj = NULL;
    pthread_mutex_unlock(&klock);
}

/* ------------------------------------------------------------------ waitable objects */
typedef struct { int manual, signalled; } Event;
typedef struct { CPU *owner; int count; } Mutex;
struct Thread {
    pthread_t pt;
    CPU *cpu;
    uint32_t start, param, exit_code;
    int finished;
};

static int signalled_locked(uint32_t h, CPU *self)
{
    switch (handle_type(h)) {
    case H_EVENT: return ((Event *)handle_get(h, H_EVENT))->signalled;
    case H_MUTEX: { Mutex *m = handle_get(h, H_MUTEX); return !m->owner || m->owner == self; }
    case H_THREAD: return ((Thread *)handle_get(h, H_THREAD))->finished;
    default: return 1;
    }
}

static void acquire_locked(uint32_t h, CPU *self)
{
    switch (handle_type(h)) {
    case H_EVENT: { Event *e = handle_get(h, H_EVENT); if (!e->manual) e->signalled = 0; break; }
    case H_MUTEX: { Mutex *m = handle_get(h, H_MUTEX); m->owner = self; m->count++; break; }
    default: break;
    }
}

int wait_handles(int n, const uint32_t *hs, int all, uint32_t ms)
{
    CPU *self = current_cpu();
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    if (ms != 0xffffffffu) {
        until.tv_sec += ms / 1000;
        until.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&klock);
    for (;;) {
        if (all) {
            int ok = 1;
            for (int i = 0; i < n; i++)
                ok &= signalled_locked(hs[i], self);
            if (ok) {
                for (int i = 0; i < n; i++)
                    acquire_locked(hs[i], self);
                pthread_mutex_unlock(&klock);
                return 0;
            }
        } else {
            for (int i = 0; i < n; i++)
                if (signalled_locked(hs[i], self)) {
                    acquire_locked(hs[i], self);
                    pthread_mutex_unlock(&klock);
                    return i;
                }
        }
        if (ms == 0) break;
        int r = ms == 0xffffffffu ? pthread_cond_wait(&kcond, &klock)
                                  : pthread_cond_timedwait(&kcond, &klock, &until);
        if (r == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&klock);
    return 0x102;                         /* WAIT_TIMEOUT */
}

int wait_handle(uint32_t h, uint32_t ms) { return wait_handles(1, &h, 0, ms); }

static void kick(void) { pthread_cond_broadcast(&kcond); }

API(KERNEL32, CreateEventA, 4)
{
    Event *e = calloc(1, sizeof *e);
    e->manual = ARG(1) != 0;
    e->signalled = ARG(2) != 0;
    uint32_t h = handle_new(H_EVENT, e);
    LOG(LOG_THREAD, "CreateEventA(manual=%d, initial=%d, %s) -> %x", e->manual, e->signalled,
        ARG(3) ? gstr(ARG(3)) : "unnamed", h);
    set_last_error(0);
    return h;
}

API(KERNEL32, SetEvent, 1)
{
    pthread_mutex_lock(&klock);
    Event *e = handle_get(ARG(0), H_EVENT);
    if (e) {
        e->signalled = 1;
        kick();
    }
    pthread_mutex_unlock(&klock);
    LOG(LOG_THREAD, "SetEvent(%x)", ARG(0));
    return e != NULL;
}

/* An event set from the runtime (DirectSound's position notifications) */
void event_signal(uint32_t h)
{
    pthread_mutex_lock(&klock);
    Event *e = handle_get(h, H_EVENT);
    if (e) {
        e->signalled = 1;
        kick();
    }
    pthread_mutex_unlock(&klock);
}

API(KERNEL32, CreateMutexA, 3)
{
    Mutex *m = calloc(1, sizeof *m);
    if (ARG(1)) {
        m->owner = current_cpu();
        m->count = 1;
    }
    uint32_t h = handle_new(H_MUTEX, m);
    LOG(LOG_THREAD, "CreateMutexA(owned=%d, %s) -> %x", ARG(1) != 0, ARG(2) ? gstr(ARG(2)) : "unnamed", h);
    set_last_error(0);                   /* the game treats any error here as "already running" */
    return h;
}

API(KERNEL32, ReleaseMutex, 1)
{
    pthread_mutex_lock(&klock);
    Mutex *m = handle_get(ARG(0), H_MUTEX);
    int ok = m && m->owner == current_cpu();
    if (ok && --m->count == 0) {
        m->owner = NULL;
        kick();
    }
    pthread_mutex_unlock(&klock);
    return ok;
}

API(KERNEL32, WaitForSingleObject, 2)
{
    LOG(LOG_THREAD, "WaitForSingleObject(%x, %d)", ARG(0), (int)ARG(1));
    int r = wait_handle(ARG(0), ARG(1));
    LOG(LOG_THREAD, "WaitForSingleObject(%x) -> %x", ARG(0), r);
    return r;
}

API(KERNEL32, WaitForMultipleObjects, 4)
{
    uint32_t n = ARG(0), hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++)
        hs[i] = R32(ARG(1) + 4 * i);
    LOG(LOG_THREAD, "WaitForMultipleObjects(%u, all=%d, %d)", n, ARG(2) != 0, (int)ARG(3));
    return wait_handles(n, hs, ARG(2) != 0, ARG(3));
}

/* ------------------------------------------------------------------ threads */
static void *thread_main(void *arg)
{
    Thread *t = arg;
    set_current_cpu(t->cpu);
    fpu_activate(t->cpu);
    LOG(LOG_THREAD, "thread %d starts at %08x(%08x)", t->cpu->thread_index, t->start, t->param);
    uint32_t r = guest_call(t->cpu, t->start, 1, t->param);
    pthread_mutex_lock(&klock);
    t->exit_code = r;
    t->finished = 1;
    kick();
    pthread_mutex_unlock(&klock);
    LOG(LOG_THREAD, "thread %d returned %u", t->cpu->thread_index, r);
    return NULL;
}

API(KERNEL32, CreateThread, 6)
{
    Thread *t = calloc(1, sizeof *t);
    t->cpu = cpu_new(ARG(1) ? ARG(1) : 0x100000);
    t->cpu->thread = t;
    t->start = ARG(2);
    t->param = ARG(3);
    if (ARG(4) & 4)
        LOG(LOG_ALWAYS, "CreateThread: CREATE_SUSPENDED not supported, starting at once");
    if (deterministic)
        LOG(LOG_ALWAYS, "CreateThread in --test mode: thread timing makes this run non-repeatable");
    uint32_t h = handle_new(H_THREAD, t);
    if (ARG(5))
        W32(ARG(5), R32(t->cpu->fs_base + 0x24));
    LOG(LOG_THREAD, "CreateThread(start=%08x, param=%08x, stack=%x) -> %x (thread %d)",
        t->start, t->param, ARG(1), h, t->cpu->thread_index);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    pthread_create(&t->pt, &at, thread_main, t);
    pthread_attr_destroy(&at);
    return h;
}

API(KERNEL32, ExitThread, 1)
{
    CPU *self = current_cpu();
    Thread *t = self->thread;
    LOG(LOG_THREAD, "ExitThread(%u) on thread %d", ARG(0), self->thread_index);
    if (!t)
        process_exit((int)ARG(0));
    pthread_mutex_lock(&klock);
    t->exit_code = ARG(0);
    t->finished = 1;
    kick();
    pthread_mutex_unlock(&klock);
    pthread_exit(NULL);
}

API(KERNEL32, SetThreadPriority, 2)
{
    LOG(LOG_THREAD, "SetThreadPriority(%x, %d)", ARG(0), (int)ARG(1));
    return 1;
}

API(KERNEL32, Sleep, 1)
{
    uint32_t ms = ARG(0);
    if (ms == 0) {
        sched_yield();
        return 0;
    }
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
    return 0;
}

API(KERNEL32, InterlockedExchange, 2)
{
    return __atomic_exchange_n((uint32_t *)gptr(ARG(0)), ARG(1), __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------ critical sections
   The program's CRITICAL_SECTION (24 bytes) keeps an index into our table at +0x10 and a
   marker at +0x14. */
#define CS_MAGIC 0x43534e4cu
#define MAXCS 1024
static pthread_mutex_t cs_tab[MAXCS];
static int ncs;

static pthread_mutex_t *cs_get(uint32_t cs)
{
    if (R32(cs + 0x14) != CS_MAGIC)
        fatal("critical section %08x used before InitializeCriticalSection", cs);
    return &cs_tab[R32(cs + 0x10)];
}

API(KERNEL32, InitializeCriticalSection, 1)
{
    uint32_t cs = ARG(0);
    pthread_mutex_lock(&klock);
    if (ncs >= MAXCS)
        fatal("too many critical sections");
    int i = ncs++;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs_tab[i], &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_unlock(&klock);
    memset(gptr(cs), 0, 24);
    W32(cs + 4, 0xffffffffu);            /* LockCount */
    W32(cs + 0x10, i);
    W32(cs + 0x14, CS_MAGIC);
    return 0;
}

API(KERNEL32, DeleteCriticalSection, 1)
{
    W32(ARG(0) + 0x14, 0);
    return 0;
}

API(KERNEL32, EnterCriticalSection, 1)
{
    pthread_mutex_lock(cs_get(ARG(0)));
    return 0;
}

API(KERNEL32, LeaveCriticalSection, 1)
{
    pthread_mutex_unlock(cs_get(ARG(0)));
    return 0;
}

void sync_init(void) {}
