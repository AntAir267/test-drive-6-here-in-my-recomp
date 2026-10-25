/* Options: <user>/options.txt, "key = value" lines, # comments. Missing keys keep the defaults.
   --option key=value on the command line overrides the file for one run. */
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"

typedef struct { char key[48], value[208]; int dirty; } Opt;
static Opt opts[64];
static int nopts;

static void set(const char *k, const char *v)
{
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, k)) {
            snprintf(opts[i].value, sizeof opts[i].value, "%s", v);
            return;
        }
    if (nopts < 64) {
        snprintf(opts[nopts].key, sizeof opts[nopts].key, "%s", k);
        snprintf(opts[nopts].value, sizeof opts[nopts].value, "%s", v);
        nopts++;
    }
}

/* "key = value" -> set; returns 0 if the line has no '=' */
int option_parse(const char *line)
{
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    char k[64], v[256];
    const char *a = line, *b = eq;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
    a = eq + 1;
    while (*a && isspace((unsigned char)*a)) a++;
    b = a + strlen(a);
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(v, sizeof v, "%.*s", (int)(b - a), a);
    set(k, v);
    return 1;
}

/* comments for keys written to the file the first time */
static const char *const help[][2] = {
    {"resolution", "resolution: original (the game's display mode, scaled up) or native (the screen's own)"},
    {"widescreen", "widescreen: off (4:3 as shipped), on (match the screen), or an aspect such as 16:9"},
    {"display", "display: window or fullscreen (Alt+Enter switches too)"},
    {"vsync", "vsync: on (frames wait for the screen) or off"},
    {"frame_limit", "frame_limit: off, screen (3 under the screen's refresh, for G-Sync / FreeSync) or a rate"},
    {"menu_fps", "menu_fps: the front end's frame rate (its animations move a step a frame, made for 60 Hz): "
                 "60, another rate, or off (as fast as the screen)"},
    {"rumble", "rumble: on (the game's own strength, 75 %), off, or a strength: 25, 50, 75 or 100"},
    {"controller_deadzone", "controller_deadzone: the sticks' dead zone against resting drift, percent"},
    {NULL, NULL}};

/* The first start (no options.txt yet, outside test runs): a file listing every option,
   commented out at its default */
static void write_template(const char *path)
{
    static const char *const def[][2] = {
        {"display", "window"}, {"resolution", "original"}, {"widescreen", "off"}, {"vsync", "on"},
        {"frame_limit", "off"}, {"menu_fps", "60"}, {"rumble", "on"}, {"controller_deadzone", "6"},
        {NULL, NULL}};
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fputs("# TD6 native: options (key = value).\n"
          "# Every option is listed at its default, commented out: take away the # to change one.\n", f);
    for (int k = 0; def[k][0]; k++) {
        for (int h = 0; help[h][0]; h++)
            if (!strcmp(help[h][0], def[k][0]))
                fprintf(f, "\n# %s\n", help[h][1]);
        fprintf(f, "#%s = %s\n", def[k][0], def[k][1]);
    }
    fclose(f);
}

void options_load(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    FILE *f = fopen(path, "r");
    if (!f && !deterministic)
        write_template(path);
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s && !option_parse(s))
            LOG(LOG_ALWAYS, "options.txt: \"%s\" is not key = value", s);
    }
    fclose(f);
}

/* Where a key was found, by the address of the caller's key string (some are asked many times a
   frame): a hint, checked; each thread its own. A key not there stays not there until one is added. */
static __thread struct { const char *key; int idx, n; } hint[32];

const char *option_str(const char *key, const char *def)
{
    unsigned h = (unsigned)((uintptr_t)key >> 3) % 32;
    if (hint[h].key == key) {
        int i = hint[h].idx;
        if (i < 0 ? hint[h].n == nopts : !strcasecmp(opts[i].key, key))
            return i < 0 ? def : opts[i].value;
    }
    int found = -1;
    for (int i = 0; i < nopts && found < 0; i++)
        if (!strcasecmp(opts[i].key, key))
            found = i;
    hint[h].key = key;
    hint[h].idx = found;
    hint[h].n = nopts;
    return found < 0 ? def : opts[found].value;
}

int option_bool(const char *key, int def)
{
    const char *v = option_str(key, NULL);
    if (!v) return def;
    return !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
}

void option_set(const char *key, const char *value)
{
    set(key, value);
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, key))
            opts[i].dirty = 1;
}


/* Rewrite options.txt keeping its comments and order: a changed key's line is replaced, keys
   the file does not have yet are appended. */
int options_save(void)
{
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        LOG(LOG_ALWAYS, "options: cannot write %s", tmp);
        return 0;
    }
    int written[64] = {0};
    char line[512];
    if (!in)
        fputs("# TD6 native: options (key = value).\n"
              "# Delete a line to get the original behaviour.\n", out);
    while (in && fgets(line, sizeof line, in)) {
        char k[64] = "";
        const char *eq = strchr(line, '=');
        const char *hash = strchr(line, '#');
        if (eq && (!hash || hash > eq)) {
            const char *a = line, *b = eq;
            while (a < b && isspace((unsigned char)*a)) a++;
            while (b > a && isspace((unsigned char)b[-1])) b--;
            snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
        }
        int done = 0;
        for (int i = 0; i < nopts && k[0]; i++)
            if (!strcasecmp(opts[i].key, k) && opts[i].dirty) {
                if (!written[i]) fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
                written[i] = done = 1;
            }
        if (!done)
            fputs(line, out);
    }
    for (int i = 0; i < nopts; i++)
        if (opts[i].dirty && !written[i]) {
            for (int k = 0; help[k][0]; k++)
                if (!strcasecmp(help[k][0], opts[i].key))
                    fprintf(out, "\n# %s\n", help[k][1]);
            fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
        }
    if (in) fclose(in);
    int ok = fclose(out) == 0 && rename(tmp, path) == 0;
    if (!ok)
        LOG(LOG_ALWAYS, "options: cannot save %s", path);
    return ok;
}
