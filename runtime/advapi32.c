/* ADVAPI32: the registry, kept in a text file in the user folder (registry.txt), and the
   user name. TD6 keeps its settings in HKCU\Software\Infogrames\Test Drive 6\0.1 and reads them
   back by enumerating the key's values. */
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include "runtime.h"

typedef struct { char key[256]; char name[128]; uint32_t type; uint32_t size; uint8_t *data; } Value;
static Value *vals;
static int nvals, capvals, dirty;
static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;

#define MAXKEYS 256
static char keys[MAXKEYS][256];        /* open key handles -> full path ("HKLM\...") */

static const char *root_name(uint32_t h)
{
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    default: return NULL;
    }
}

static const char *key_path(uint32_t h)
{
    const char *r = root_name(h);
    if (r) return r;
    uint32_t i = h - 0x5000u;
    if (i < MAXKEYS && keys[i][0]) return keys[i];
    return NULL;
}

static Value *find(const char *key, const char *name)
{
    for (int i = 0; i < nvals; i++)
        if (!strcasecmp(vals[i].key, key) && !strcasecmp(vals[i].name, name))
            return &vals[i];
    return NULL;
}

static void set(const char *key, const char *name, uint32_t type, const void *data, uint32_t size)
{
    Value *v = find(key, name);
    if (!v) {
        if (nvals == capvals) {
            capvals = capvals ? capvals * 2 : 64;
            vals = realloc(vals, capvals * sizeof *vals);
        }
        v = &vals[nvals++];
        memset(v, 0, sizeof *v);
        snprintf(v->key, sizeof v->key, "%s", key);
        snprintf(v->name, sizeof v->name, "%s", name);
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    memcpy(v->data, data, size);
    dirty = 1;
}

/* registry.txt: one value per line: key<TAB>name<TAB>type<TAB>hex bytes */
void settings_load(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/registry.txt", user_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[8192];
        while (fgets(line, sizeof line, f)) {
            char *k = strtok(line, "\t"), *n = strtok(NULL, "\t"), *t = strtok(NULL, "\t"), *h = strtok(NULL, "\t\r\n");
            if (!k || !n || !t) continue;
            uint8_t buf[4096];
            uint32_t sz = 0;
            for (char *p = h; p && p[0] && p[1] && sz < sizeof buf; p += 2) {
                unsigned b;
                sscanf(p, "%2x", &b);
                buf[sz++] = (uint8_t)b;
            }
            set(k, !strcmp(n, "@") ? "" : n, strtoul(t, NULL, 10), buf, sz);
        }
        fclose(f);
    }
    dirty = 0;
}

/* Host-side access for the Enhanced page (the language choice). */
int registry_get_str(const char *key, const char *name, char *out, int n)
{
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    if (v && n > 0)
        snprintf(out, (size_t)n, "%.*s", (int)v->size, (const char *)v->data);
    pthread_mutex_unlock(&rlock);
    return v != NULL;
}

void registry_set_str(const char *key, const char *name, const char *s)
{
    pthread_mutex_lock(&rlock);
    set(key, name, 1, s, (uint32_t)strlen(s) + 1);
    pthread_mutex_unlock(&rlock);
}

void settings_save(void)
{
    pthread_mutex_lock(&rlock);
    if (dirty) {
        char path[1200], tmp[1300];
        snprintf(path, sizeof path, "%s/registry.txt", user_dir);
        snprintf(tmp, sizeof tmp, "%s.new", path);
        FILE *f = fopen(tmp, "w");
        if (f) {
            for (int i = 0; i < nvals; i++) {
                fprintf(f, "%s\t%s\t%u\t", vals[i].key, vals[i].name[0] ? vals[i].name : "@", vals[i].type);
                for (uint32_t k = 0; k < vals[i].size; k++)
                    fprintf(f, "%02x", vals[i].data[k]);
                fprintf(f, "\n");
            }
            fclose(f);
            rename(tmp, path);
            dirty = 0;
        }
    }
    pthread_mutex_unlock(&rlock);
}

/* The runtime's own access to the store (runtime.h) */
uint32_t settings_get(const char *key, const char *name, uint32_t *type, void *buf, uint32_t cap)
{
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    uint32_t n = v ? v->size : 0;
    if (v) {
        if (type) *type = v->type;
        if (buf) memcpy(buf, v->data, n < cap ? n : cap);
    }
    pthread_mutex_unlock(&rlock);
    return n;
}

int settings_has(const char *key, const char *name)
{
    pthread_mutex_lock(&rlock);
    int r = find(key, name) != NULL;
    pthread_mutex_unlock(&rlock);
    return r;
}

void settings_put(const char *key, const char *name, uint32_t type, const void *data, uint32_t size)
{
    pthread_mutex_lock(&rlock);
    set(key, name, type, data, size);
    pthread_mutex_unlock(&rlock);
}

void settings_delete(const char *key, const char *name)
{
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    if (v) {
        free(v->data);
        *v = vals[--nvals];
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

static uint32_t open_key(uint32_t parent, const char *sub, uint32_t out)
{
    const char *base = key_path(parent);
    if (!base) return 6;
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", base, sub && *sub ? "\\" : "", sub ? sub : "");
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
    pthread_mutex_lock(&rlock);
    int slot = -1;
    for (int i = 0; i < MAXKEYS; i++)
        if (!keys[i][0]) { slot = i; break; }
    if (slot >= 0)
        snprintf(keys[slot], sizeof keys[slot], "%s", full);
    pthread_mutex_unlock(&rlock);
    if (slot < 0) return 14;
    W32(out, 0x5000u + slot);
    return 0;
}

static int key_known(const char *full)
{
    size_t n = strlen(full);
    for (int i = 0; i < nvals; i++)
        if (!strncasecmp(vals[i].key, full, n) && (vals[i].key[n] == 0 || vals[i].key[n] == '\\'))
            return 1;
    return 0;
}

API(ADVAPI32, RegOpenKeyExA, 5)
{
    const char *base = key_path(ARG(0));
    const char *sub = gstr(ARG(1));
    char full[256];
    snprintf(full, sizeof full, "%s%s%s", base ? base : "?", sub && *sub ? "\\" : "", sub ? sub : "");
    pthread_mutex_lock(&rlock);
    int known = key_known(full);
    pthread_mutex_unlock(&rlock);
    uint32_t r = known ? open_key(ARG(0), sub, ARG(4)) : 2;
    LOG(LOG_REG, "RegOpenKeyExA(%s) -> %u", full, r);
    return r;
}

API(ADVAPI32, RegCreateKeyExA, 9)
{
    uint32_t r = open_key(ARG(0), gstr(ARG(1)), ARG(7));
    if (ARG(8)) W32(ARG(8), 2);          /* REG_OPENED_EXISTING_KEY */
    LOG(LOG_REG, "RegCreateKeyExA(%s\\%s) -> %u", key_path(ARG(0)), gstr(ARG(1)), r);
    return r;
}

API(ADVAPI32, RegCloseKey, 1)
{
    uint32_t i = ARG(0) - 0x5000u;
    if (i < MAXKEYS) keys[i][0] = 0;
    return 0;
}

API(ADVAPI32, RegFlushKey, 1)
{
    settings_save();
    return 0;
}

API(ADVAPI32, RegQueryValueExA, 6)
{
    const char *key = key_path(ARG(0));
    const char *name = ARG(1) ? gstr(ARG(1)) : "";
    uint32_t typep = ARG(3), data = ARG(4), sizep = ARG(5);
    if (!key) return 6;
    pthread_mutex_lock(&rlock);
    Value *v = find(key, name);
    uint32_t r;
    if (!v) {
        r = 2;
    } else {
        if (typep) W32(typep, v->type);
        uint32_t cap = sizep ? R32(sizep) : 0;
        if (data && cap < v->size) r = 234;
        else {
            if (data) memcpy(gptr(data), v->data, v->size);
            r = 0;
        }
        if (sizep) W32(sizep, v->size);
    }
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegQueryValueExA(%s, %s) -> %u", key, name, r);
    return r;
}

API(ADVAPI32, RegSetValueExA, 6)
{
    const char *key = key_path(ARG(0));
    if (!key) return 6;
    const char *name = ARG(1) ? gstr(ARG(1)) : "";
    pthread_mutex_lock(&rlock);
    set(key, name, ARG(3), gptr(ARG(4)), ARG(5));
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegSetValueExA(%s, %s, type %u, %u bytes)", key, name, ARG(3), ARG(5));
    return 0;
}

API(ADVAPI32, GetUserNameA, 2)
{
    const char *u = getenv("USER");
    if (!u || !*u)
        u = getenv("USERNAME");             /* Windows */
    if (deterministic)
        u = NULL;                           /* --test runs: the same on every machine and account */
    char name[64];
    snprintf(name, sizeof name, "%s", u && *u ? u : "Player");
    name[0] = (char)toupper((unsigned char)name[0]);
    uint32_t cap = R32(ARG(1)), n = strlen(name) + 1;
    W32(ARG(1), n);
    if (cap < n) { set_last_error(122); return 0; }
    gstrncpy(ARG(0), name, cap);
    return 1;
}

/* The values of one key in the order they were made (TD6 reads its settings this way) */
API(ADVAPI32, RegEnumValueA, 8)
{
    const char *key = key_path(ARG(0));
    if (!key) return 6;
    uint32_t idx = ARG(1), namep = ARG(2), namelenp = ARG(3), typep = ARG(5), data = ARG(6), sizep = ARG(7);
    pthread_mutex_lock(&rlock);
    Value *v = NULL;
    for (int i = 0, k = 0; i < nvals; i++)
        if (!strcasecmp(vals[i].key, key) && k++ == (int)idx) {
            v = &vals[i];
            break;
        }
    uint32_t r = 259;                   /* ERROR_NO_MORE_ITEMS */
    if (v) {
        uint32_t cap = R32(namelenp), n = (uint32_t)strlen(v->name);
        if (cap <= n)
            r = 234;
        else {
            gstrncpy(namep, v->name, cap);
            W32(namelenp, n);
            if (typep) W32(typep, v->type);
            r = 0;
            if (sizep) {
                uint32_t dcap = R32(sizep);
                if (data && dcap < v->size) r = 234;
                else if (data) memcpy(gptr(data), v->data, v->size);
                W32(sizep, v->size);
            }
        }
    }
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegEnumValueA(%s, %u) -> %u%s%s", key, idx, r, v ? " " : "", v ? v->name : "");
    return r;
}

/* The key's direct subkeys, as the stored values' key paths show them */
API(ADVAPI32, RegEnumKeyExA, 8)
{
    const char *key = key_path(ARG(0));
    if (!key) return 6;
    uint32_t idx = ARG(1);
    size_t kn = strlen(key);
    char seen[64][128];
    int nseen = 0;
    uint32_t r = 259;
    pthread_mutex_lock(&rlock);
    for (int i = 0; i < nvals && r == 259; i++) {
        const char *p = vals[i].key;
        if (strncasecmp(p, key, kn) || p[kn] != '\\')
            continue;
        char sub[128];
        snprintf(sub, sizeof sub, "%s", p + kn + 1);
        char *bs = strchr(sub, '\\');
        if (bs) *bs = 0;
        int dup = 0;
        for (int j = 0; j < nseen && !dup; j++)
            dup = !strcasecmp(seen[j], sub);
        if (dup || nseen >= 64)
            continue;
        snprintf(seen[nseen++], 128, "%s", sub);
        if ((uint32_t)nseen - 1 == idx) {
            uint32_t cap = R32(ARG(3)), n = (uint32_t)strlen(sub);
            if (cap <= n) r = 234;
            else {
                gstrncpy(ARG(2), sub, cap);
                W32(ARG(3), n);
                r = 0;
            }
        }
    }
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegEnumKeyExA(%s, %u) -> %u", key, idx, r);
    return r;
}

/* A key and its values (TD6 only deletes keys it has emptied of subkeys) */
API(ADVAPI32, RegDeleteKeyA, 2)
{
    const char *base = key_path(ARG(0));
    if (!base) return 6;
    char full[256];
    snprintf(full, sizeof full, "%s\\%s", base, gstr(ARG(1)));
    size_t n = strlen(full);
    int any = 0;
    pthread_mutex_lock(&rlock);
    for (int i = 0; i < nvals;)
        if (!strncasecmp(vals[i].key, full, n) && (vals[i].key[n] == 0 || vals[i].key[n] == '\\')) {
            free(vals[i].data);
            vals[i] = vals[--nvals];
            any = dirty = 1;
        } else
            i++;
    pthread_mutex_unlock(&rlock);
    LOG(LOG_REG, "RegDeleteKeyA(%s) -> %d", full, any);
    return any ? 0 : 2;
}
