/* Test Drive 6 - Here In My Recomp: the launcher and first-run setup (the AppImage's program).

   The download holds no part of the game: this launcher, the translator (tools/recomp.py) and the
   installer (tools/install.py) on a Python of their own, a C compiler (Zig), and the runtime
   built ahead of time (lib/: libruntime.a, SDL3, a small FFmpeg; include/: the headers the
   generated code includes). The first time it runs it asks for the player's Test Drive 6 CD (the
   disc, a copy of its files, an .iso or a .zip of one), copies the game's files from it into the
   user folder (game/, install.py: TD6.exe checked against the known release), translates TD6.exe
   to C (build/gen), compiles that and links it with the runtime (bin/td6). After that it starts
   the game at once, until a new download (another VERSION) builds it again.

   Bundle: <launcher's folder>/../lib/td6-recomp, on Windows the launcher's own folder (or
   TD6_BUNDLE): VERSION, FUNCS_SHA256, zig/, python/, tools/, builds/, include/, lib/. User folder:
   the game's own, ~/.local/share/td6-native or %APPDATA%\td6-native (TD6_USER for another).

   Without a window (--setup-from <CD, folder, .iso or .zip> [--no-launch]) it does the same in the
   terminal: for tests and for those who prefer it. --setup shows the setup again. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define EXE ".exe"
#define TARGET "x86_64-windows-gnu"
#define PYTHON "python/python.exe"
#else
#define EXE ""
#define TARGET "x86_64-linux-gnu.2.31"
#define PYTHON "python/bin/python3.14"
#endif

static char bundle[1024], user[1024];
static char version[128], release[32];

/* ------------------------------------------------------------------ the shared state */
static SDL_Mutex *lock;
static char status[256], detail[1024];
static double progress = -1;            /* 0..1, or < 0 for none */
static volatile int task_done = 1, task_ok = 1;
static int headless;

static void say(const char *st, const char *de)
{
    SDL_LockMutex(lock);
    if (st)
        SDL_strlcpy(status, st, sizeof status);
    if (de)
        SDL_strlcpy(detail, de, sizeof detail);
    SDL_UnlockMutex(lock);
    if (headless)
        SDL_Log("%s%s%s", st ? st : "", st && de && *de ? ": " : "", de ? de : "");
}

static void set_progress(double p)
{
    SDL_LockMutex(lock);
    progress = p;
    SDL_UnlockMutex(lock);
}

/* ------------------------------------------------------------------ files */
static void path(char *out, size_t n, const char *a, const char *b)
{
    SDL_snprintf(out, n, "%s/%s", a, b);
}

static int exists(const char *p)
{
    SDL_PathInfo i;
    return SDL_GetPathInfo(p, &i);
}

static char *read_text(const char *p)
{
    size_t n;
    return SDL_LoadFile(p, &n);
}

static int write_text(const char *p, const char *s)
{
    return SDL_SaveFile(p, s, SDL_strlen(s));
}

static void make_dirs(const char *p)
{
    char t[1024];
    SDL_strlcpy(t, p, sizeof t);
    for (char *c = t + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            SDL_CreateDirectory(t);
            *c = '/';
        }
    SDL_CreateDirectory(t);
}

static SDL_EnumerationResult remove_entry(void *ud, const char *dir, const char *name);

static void remove_tree(const char *p)
{
    SDL_EnumerateDirectory(p, remove_entry, NULL);
    SDL_RemovePath(p);
}

static SDL_EnumerationResult remove_entry(void *ud, const char *dir, const char *name)
{
    (void)ud;
    char p[1024];
    SDL_PathInfo i;
    SDL_snprintf(p, sizeof p, "%s%s", dir, name);   /* (dir ends in a separator) */
    if (SDL_GetPathInfo(p, &i) && i.type == SDL_PATHTYPE_DIRECTORY)
        remove_tree(p);
    else
        SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

/* name in folder, ignoring case, into out */
typedef struct { const char *want; char found[256]; } FindName;

static SDL_EnumerationResult find_cb(void *ud, const char *dir, const char *name)
{
    (void)dir;
    FindName *f = ud;
    if (!SDL_strcasecmp(name, f->want)) {
        SDL_strlcpy(f->found, name, sizeof f->found);
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

static int child_ci(const char *dir, const char *name, char *out, size_t n)
{
    FindName f = {name, ""};
    SDL_EnumerateDirectory(dir, find_cb, &f);
    if (!f.found[0])
        return 0;
    SDL_snprintf(out, n, "%s/%s", dir, f.found);
    return 1;
}

/* a CD (or a copy of one, or an installed game): TD6.exe in it or in InstData/GameData/Common */
static int is_td6_folder(const char *dir)
{
    char a[1024], b[1024], c[1024], e[1024];
    if (child_ci(dir, "TD6.exe", e, sizeof e) && child_ci(dir, "frontend.zip", e, sizeof e))
        return 1;
    return child_ci(dir, "InstData", a, sizeof a) && child_ci(a, "GameData", b, sizeof b) &&
           child_ci(b, "Common", c, sizeof c) && child_ci(c, "TD6.exe", e, sizeof e);
}

/* ------------------------------------------------------------------ running the tools */
/* Runs argv, waits; its output (stdout and stderr) goes to the setup log, and its last lines are
   kept for an error message. Returns its exit code (-1: not started). */
static char last_output[2048];

static void log_run(const char *const *argv, const char *out, int code)
{
    char log[1024];
    path(log, sizeof log, user, "setup.log");
    SDL_IOStream *lf = SDL_IOFromFile(log, "ab");
    if (!lf)
        return;
    SDL_IOprintf(lf, "$");
    for (int i = 0; argv[i]; i++)
        SDL_IOprintf(lf, " %s", argv[i]);
    SDL_IOprintf(lf, "\n%s\n[exit %d]\n", out ? out : "", code);
    SDL_CloseIO(lf);
}

static SDL_Process *spawn(const char *const *argv)
{
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    return p;
}

static void keep_tail(const char *out, size_t n)
{
    last_output[0] = 0;
    if (out) {
        size_t k = n > sizeof last_output - 1 ? n - (sizeof last_output - 1) : 0;
        SDL_strlcpy(last_output, out + k, sizeof last_output);
    }
}

static int run(const char *const *argv)
{
    SDL_Process *p = spawn(argv);
    if (!p) {
        SDL_snprintf(last_output, sizeof last_output, "cannot start %s: %s", argv[0], SDL_GetError());
        return -1;
    }
    size_t n = 0;
    int code = -1;
    char *out = SDL_ReadProcess(p, &n, &code);
    log_run(argv, out, code);
    keep_tail(out, n);
    SDL_free(out);
    SDL_DestroyProcess(p);
    return code;
}

/* the bundle's Python running one of its tools */
static int run_tool(const char *tool, const char *const *args)
{
    char py[1024], t[1024];
    path(py, sizeof py, bundle, PYTHON);
    SDL_snprintf(t, sizeof t, "%s/tools/%s", bundle, tool);
    const char *argv[16] = {py, "-E", "-s", "-B", t};
    int k = 5;
    for (int i = 0; args[i] && k < 15; i++)
        argv[k++] = args[i];
    argv[k] = NULL;
    return run(argv);
}

/* ------------------------------------------------------------------ the steps */
static int game_installed(void)
{
    char g[1024], e[1024];
    path(g, sizeof g, user, "game");
    return child_ci(g, "TD6.exe", e, sizeof e) && child_ci(g, "frontend.zip", e, sizeof e);
}

/* the game's files from the CD, a folder or an image into game/ (tools/install.py) */
static int install(const char *src)
{
    char g[1024];
    path(g, sizeof g, user, "game");
    say("Copying the game's files", "from your Test Drive 6 CD (nothing on it is run, only read)");
    set_progress(-1);
    remove_tree(g);                     /* (what a stopped run left) */
    const char *args[] = {src, "--to", g, NULL};
    int code = run_tool("install.py", args);
    if (code != 0) {
        remove_tree(g);
        say(code == 2 ? "This TD6.exe isn't the known release" : "Could not copy the game's files", last_output);
        return 0;
    }
    return 1;
}

/* TD6.exe to C, then compiled (several at once) and linked with the runtime */
static int build_game(void)
{
    char zig[1024], gen[1024], obj[1024], bin[1024], cfg[1024], exe[1024], inc[1024], cache[1024];
    path(zig, sizeof zig, bundle, "zig/zig" EXE);
    path(gen, sizeof gen, user, "build/gen");
    path(obj, sizeof obj, user, "build/obj");
    path(bin, sizeof bin, user, "bin");
    path(cfg, sizeof cfg, bundle, "builds/td6_1999.py");
    path(inc, sizeof inc, bundle, "include");
    path(cache, sizeof cache, user, "build/zig-cache");
    char g[1024];
    path(g, sizeof g, user, "game");
    if (!child_ci(g, "TD6.exe", exe, sizeof exe)) {
        say("The game's files are missing", "TD6.exe is not in the user folder's game/");
        return 0;
    }
    make_dirs(gen);
    make_dirs(obj);
    make_dirs(bin);
    make_dirs(cache);                   /* (before the compilers start: they race to make it otherwise) */
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "ZIG_GLOBAL_CACHE_DIR", cache, true);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "ZIG_LOCAL_CACHE_DIR", cache, true);

    say("Translating the game", "TD6.exe to C, on this computer (a few seconds)");
    set_progress(-1);
    const char *targs[] = {exe, cfg, gen, NULL};
    if (run_tool("recomp.py", targs) != 0) {
        say("Translating failed", last_output);
        return 0;
    }
    /* the runtime was built against this very translation: its declarations must match */
    char want_p[1024], have_p[1024];
    path(want_p, sizeof want_p, bundle, "FUNCS_SHA256");
    path(have_p, sizeof have_p, gen, "funcs.sha256");
    char *want = read_text(want_p);
    {
        /* (sha256 of gen/funcs.h, by the bundle's Python: no other tool needed) */
        char fh[1024], py[1024];
        path(fh, sizeof fh, gen, "funcs.h");
        path(py, sizeof py, bundle, PYTHON);
        char code[1400];
        SDL_snprintf(code, sizeof code,
                     "import hashlib;open(r'%s','w').write(hashlib.sha256(open(r'%s','rb').read()).hexdigest())", have_p, fh);
        const char *argv[] = {py, "-E", "-s", "-c", code, NULL};
        run(argv);
    }
    char *have = read_text(have_p);
    int match = want && have && !SDL_strncmp(want, have, 64);
    SDL_free(want);
    SDL_free(have);
    if (!match) {
        say("The translation doesn't match this download", "please report it (setup.log in the user folder)");
        return 0;
    }

    /* the generated files */
    static char names[128][32];
    int nnames = 0;
    for (int i = 0; i < 120; i++) {
        char f[32], p[1024];
        SDL_snprintf(f, sizeof f, "code_%03d.c", i);
        path(p, sizeof p, gen, f);
        if (!exists(p))
            break;
        SDL_strlcpy(names[nnames++], f, sizeof names[0]);
    }
    SDL_strlcpy(names[nnames++], "table.c", sizeof names[0]);
    SDL_strlcpy(names[nnames++], "stubs.c", sizeof names[0]);

    int jobs = SDL_GetNumLogicalCPUCores();
    jobs = jobs > 2 ? jobs - 1 : 1;
    if (SDL_getenv("TD6_JOBS"))         /* (fewer, to keep the computer free for other things) */
        jobs = SDL_atoi(SDL_getenv("TD6_JOBS"));
    if (jobs < 1)
        jobs = 1;
    if (jobs > 16)
        jobs = 16;
    SDL_Process *procs[16] = {0};
    int which[16];
    int next = 0, done = 0, failed = 0;
    static char objs[128][1024];
    say("Building the game", "compiling the translated code (a few minutes, once)");
    while (done < nnames && !failed) {
        for (int j = 0; j < jobs && next < nnames; j++) {
            if (procs[j])
                continue;
            char src[1024];
            path(src, sizeof src, gen, names[next]);
            SDL_snprintf(objs[next], sizeof objs[0], "%s/%.*s.o", obj, (int)(SDL_strlen(names[next]) - 2), names[next]);
            const char *argv[] = {zig, "cc", "-target", TARGET, "-std=gnu11", "-O2", "-w", "-DCPU_FAST_MEM",
                                  "-fno-strict-aliasing", "-fno-omit-frame-pointer", "-fno-slp-vectorize",
#ifndef _WIN32
                                  "-fPIE",
#endif
                                  "-I", inc, "-I", gen, "-c", src, "-o", objs[next], NULL};
            procs[j] = spawn(argv);
            if (!procs[j]) {
                SDL_snprintf(last_output, sizeof last_output, "cannot start the compiler: %s", SDL_GetError());
                failed = 1;
                break;
            }
            which[j] = next++;
        }
        for (int j = 0; j < jobs; j++) {
            int code;
            if (procs[j] && SDL_WaitProcess(procs[j], false, &code)) {
                size_t n;
                char *out = SDL_ReadProcess(procs[j], &n, NULL);
                if (code != 0) {
                    SDL_snprintf(last_output, sizeof last_output, "%s: %s", names[which[j]], out ? out : "");
                    failed = 1;
                }
                SDL_free(out);
                SDL_DestroyProcess(procs[j]);
                procs[j] = NULL;
                done++;
                set_progress(0.9 * done / nnames);
                char m[256];
                SDL_snprintf(m, sizeof m, "compiled %d of %d parts", done, nnames);
                say(NULL, m);
            }
        }
        SDL_Delay(50);
    }
    for (int j = 0; j < jobs; j++)
        if (procs[j]) {
            SDL_WaitProcess(procs[j], true, NULL);
            SDL_DestroyProcess(procs[j]);
        }
    if (failed) {
        say("Compiling failed", last_output);
        return 0;
    }

    say("Building the game", "linking it with the runtime");
    char out[1024], lib[1024], rt[1024], stubs[1024];
    path(out, sizeof out, bin, "td6" EXE);
    path(lib, sizeof lib, bundle, "lib");
    path(rt, sizeof rt, bundle, "lib/libruntime.a");
    path(stubs, sizeof stubs, bundle, "lib/stubs");
    const char *largv[200];
    int k = 0;
    largv[k++] = zig, largv[k++] = "cc", largv[k++] = "-target", largv[k++] = TARGET;
#ifndef _WIN32
    largv[k++] = "-fPIE", largv[k++] = "-pie", largv[k++] = "-rdynamic";
#endif
    largv[k++] = "-o", largv[k++] = out;
    for (int i = 0; i < nnames; i++)
        largv[k++] = objs[i];
    largv[k++] = "-Wl,--whole-archive", largv[k++] = rt, largv[k++] = "-Wl,--no-whole-archive";
    largv[k++] = "-L", largv[k++] = lib, largv[k++] = "-L", largv[k++] = stubs;
    largv[k++] = "-lSDL3", largv[k++] = "-lavformat", largv[k++] = "-lavcodec", largv[k++] = "-lswscale";
    largv[k++] = "-lswresample", largv[k++] = "-lavutil";
#ifdef _WIN32
    static char res[1024];              /* the manifest and the icon */
    path(res, sizeof res, bundle, "lib/td6.res");
    if (exists(res))
        largv[k++] = res;
    static const char *const winlibs[] = {"-lz", "-lvulkan-1", "-luser32", "-lgdi32", "-lwinmm", "-limm32", "-lole32",
        "-loleaut32", "-lversion", "-luuid", "-ladvapi32", "-lsetupapi", "-lshell32", "-ldinput8", "-lbcrypt",
        "-lpthread", "-static", "-Wl,--stack,8388608", "-mwindows", NULL};
    for (int i = 0; winlibs[i]; i++)
        largv[k++] = winlibs[i];
#else
    largv[k++] = "-lvulkan", largv[k++] = "-lz", largv[k++] = "-lm", largv[k++] = "-ldl", largv[k++] = "-pthread";
    largv[k++] = "-Wl,--allow-shlib-undefined";
#endif
    largv[k] = NULL;
    if (run(largv) != 0) {
        say("Linking failed", last_output);
        return 0;
    }
    char stamp[1024], build[1024];
    path(stamp, sizeof stamp, bin, "VERSION");
    write_text(stamp, version);
    path(build, sizeof build, user, "build");
    remove_tree(build);                 /* (the C and the objects: about 100 MB, not needed now) */
    set_progress(1);
    say("Built", "Test Drive 6 is ready");
    return 1;
}

/* ------------------------------------------------------------------ tasks */
static char source[1024];               /* what the player chose: a folder or an image */

static int setup_task(void *unused)
{
    (void)unused;
    int ok = 1;
    if (source[0])
        ok = install(source);
    else if (!game_installed()) {
        say("The game's files are missing", "choose your Test Drive 6 CD");
        ok = 0;
    }
    if (ok)
        ok = build_game();
    task_ok = ok;
    task_done = 1;
    return 0;
}

static void start(SDL_ThreadFunction fn)
{
    task_done = 0;
    task_ok = 1;
    SDL_Thread *t = SDL_CreateThread(fn, "setup", NULL);
    SDL_DetachThread(t);
}

static int built(void)
{
    char stamp[1024], g[1024];
    path(stamp, sizeof stamp, user, "bin/VERSION");
    char *s = read_text(stamp);
    int ok = s && !SDL_strcmp(s, version);
    SDL_free(s);
    path(g, sizeof g, user, "bin/td6" EXE);
    return ok && exists(g) && game_installed();
}

static void launch_game(int argc, char **argv)
{
    char g[1024], log[1024], game[1024];
    path(g, sizeof g, user, "bin/td6" EXE);
    path(log, sizeof log, user, "last-run.log");
    path(game, sizeof game, user, "game");
    const char *args[64];
    int k = 0;
    args[k++] = g;
    args[k++] = "--window";             /* (without it the game runs headless, as for tests) */
    args[k++] = "--user", args[k++] = user, args[k++] = "--game", args[k++] = game;
    args[k++] = "--logfile", args[k++] = log;
    for (int i = 1; i < argc && k < 62; i++)
        if (SDL_strcmp(argv[i], "--setup"))
            args[k++] = argv[i];
    args[k] = NULL;
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (p)
        SDL_DestroyProcess(p);
}

/* where a copy is often found: the game folder already filled, a CD in the drive */
typedef struct { char *out; size_t n; int found; } FindCd;

static SDL_EnumerationResult find_cd_cb(void *ud, const char *dir, const char *name)
{
    FindCd *f = ud;
    char p[1024];
    SDL_snprintf(p, sizeof p, "%s%s", dir, name);
    if (is_td6_folder(p)) {
        SDL_strlcpy(f->out, p, f->n);
        f->found = 1;
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

static int detect(char *out, size_t n)
{
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();  /* a CD in a drive (or a mounted image): D: to Z: */
    for (int d = 3; d < 26; d++) {
        char root[4] = {(char)('A' + d), ':', 0};
        if ((drives >> d & 1) && GetDriveTypeA((char[]){root[0], ':', '\\', 0}) == DRIVE_CDROM && is_td6_folder(root)) {
            SDL_strlcpy(out, root, n);
            return 1;
        }
    }
    return 0;
#endif
    const char *u = SDL_getenv("USER");
    char places[3][512];
    SDL_snprintf(places[0], sizeof places[0], "/run/media/%s", u ? u : "");
    SDL_snprintf(places[1], sizeof places[1], "/media/%s", u ? u : "");
    SDL_strlcpy(places[2], "/media", sizeof places[2]);
    for (int i = 0; i < 3; i++) {
        FindCd f = {out, n, 0};
        if (exists(places[i]))
            SDL_EnumerateDirectory(places[i], find_cd_cb, &f);
        if (f.found)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ drawing (SDL's own 8x8 font) */
static SDL_Window *win;
static SDL_Renderer *ren;
static float unit = 1;                  /* pixels per layout unit (the layout is 1000 x 600) */

static void color(Uint32 rgb, Uint8 a)
{
    SDL_SetRenderDrawColor(ren, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, a);
}

static void rect(float x, float y, float w, float h, Uint32 rgb)
{
    color(rgb, 255);
    SDL_FRect r = {x * unit, y * unit, w * unit, h * unit};
    SDL_RenderFillRect(ren, &r);
}

/* text at layout (x, y), size = the letters' height in layout units; returns its width */
static float text(float x, float y, const char *s, float size, Uint32 rgb)
{
    float k = size * unit / 8.0f;
    if (k < 1)
        k = 1;
    SDL_SetRenderScale(ren, k, k);
    color(rgb, 255);
    SDL_RenderDebugText(ren, x * unit / k, y * unit / k, s);
    SDL_SetRenderScale(ren, 1, 1);
    return (float)SDL_strlen(s) * 8.0f * k / unit;
}

static float text_wrapped(float x, float y, float width, const char *s, float size, Uint32 rgb)
{
    float k = size * unit / 8.0f;
    if (k < 1)
        k = 1;
    int per_line = (int)(width * unit / (8.0f * k));
    if (per_line < 8)
        per_line = 8;
    float yy = y;
    const char *p = s;
    char line[512];
    while (*p) {
        int len = (int)strcspn(p, "\n"), cut = len;
        if (cut > per_line) {
            cut = per_line;
            while (cut > 0 && p[cut] != ' ')
                cut--;
            if (cut == 0)
                cut = per_line;
        }
        if (cut >= (int)sizeof line)
            cut = (int)sizeof line - 1;
        SDL_memcpy(line, p, (size_t)cut);
        line[cut] = 0;
        text(x, yy, line, size, rgb);
        yy += size * 1.5f;
        p += cut;
        while (*p == ' ' || *p == '\n')
            p++;
    }
    return yy - y;
}

/* ------------------------------------------------------------------ the pages */
enum { P_WELCOME, P_WORK, P_DONE, P_ERROR };
enum { B_FOUND, B_IMAGE, B_FOLDER, B_PLAY, B_QUIT, B_BACK };
static int page = P_WELCOME, sel = 0;
static char found[1024];
static int have_found;
static int buttons[8], nbuttons;
static SDL_FRect button_rects[8];
static volatile int dialog_done;
static char dialog_result[1024];

static void SDLCALL dialog_cb(void *ud, const char *const *files, int filter)
{
    (void)ud, (void)filter;
    if (files && files[0])
        SDL_strlcpy(dialog_result, files[0], sizeof dialog_result);
    dialog_done = 1;
}

static const char *button_label(int b)
{
    switch (b) {
    case B_FOUND: return "Use the CD found here";
    case B_IMAGE: return "Choose a disc image (.iso, or a .zip holding one)";
    case B_FOLDER: return "Choose the CD's folder (or a copy of its files)";
    case B_PLAY: return "Play";
    case B_QUIT: return "Quit";
    case B_BACK: return "Back";
    }
    return "";
}

static void press(int b)
{
    switch (b) {
    case B_FOUND:
        SDL_strlcpy(source, found, sizeof source);
        page = P_WORK;
        start(setup_task);
        break;
    case B_IMAGE: {
        static const SDL_DialogFileFilter f[] = {{"Disc image", "iso;zip"}};
        dialog_done = 0, dialog_result[0] = 0;
        SDL_ShowOpenFileDialog(dialog_cb, NULL, win, f, 1, NULL, false);
        break;
    }
    case B_FOLDER:
        dialog_done = 0, dialog_result[0] = 0;
        SDL_ShowOpenFolderDialog(dialog_cb, NULL, win, NULL, false);
        break;
    case B_BACK:
        page = P_WELCOME;
        break;
    }
}

static void layout(void)
{
    nbuttons = 0;
    if (page == P_WELCOME) {
        if (have_found)
            buttons[nbuttons++] = B_FOUND;
        buttons[nbuttons++] = B_IMAGE;
        buttons[nbuttons++] = B_FOLDER;
        buttons[nbuttons++] = B_QUIT;
    } else if (page == P_DONE) {
        buttons[nbuttons++] = B_PLAY;
        buttons[nbuttons++] = B_QUIT;
    } else if (page == P_ERROR) {
        buttons[nbuttons++] = B_BACK;
        buttons[nbuttons++] = B_QUIT;
    }
    if (sel >= nbuttons)
        sel = nbuttons - 1;
    if (sel < 0)
        sel = 0;
}

static void draw(void)
{
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    unit = (float)wh / 600.0f;
    float W = ww / unit;
    color(0x0c1020, 255);
    SDL_RenderClear(ren);
    rect(0, 0, W, 6, 0xd02020);
    float tw = text(48, 36, "Test Drive 6 - Here In My Recomp", 24, 0xffd040);
    text(48 + tw + 16, 46, release, 14, 0x9098b0);
    char st[256], de[1024];
    double pr;
    SDL_LockMutex(lock);
    SDL_strlcpy(st, status, sizeof st);
    SDL_strlcpy(de, detail, sizeof de);
    pr = progress;
    SDL_UnlockMutex(lock);
    float y = 104;
    if (page == P_WELCOME) {
        y += text_wrapped(48, y, W - 96,
                          "The 1999 racer, rebuilt for today's computers. It needs your own Test Drive 6 CD for "
                          "PC: this builds the game from it, once, on this computer.", 16, 0xe8e8f0);
        y += 14;
        if (have_found) {
            char m[1200];
            SDL_snprintf(m, sizeof m, "Found: %s", found);
            text_wrapped(48, y, W - 96, m, 14, 0x9ad0ff);
        } else
            text_wrapped(48, y, W - 96, "Show it where your copy is: the CD, a copy of its files, or a disc image.",
                         14, 0xb8b8c8);
    } else {
        text(48, y, st, 22, page == P_ERROR ? 0xff7060 : 0xffffff);
        y += 40;
        text_wrapped(48, y, W - 96, de, 13, 0xb8b8c8);
        if (page == P_WORK) {
            rect(48, 300, W - 96, 14, 0x303850);
            if (pr >= 0)
                rect(48, 300, (float)((W - 96) * pr), 14, 0xffd040);
            else {
                float t = (float)(SDL_GetTicks() % 1600) / 1600.0f;
                rect(48 + (W - 96 - 120) * t, 300, 120, 14, 0xffd040);
            }
        }
    }
    layout();
    float by = 600 - 64.0f * nbuttons - 24;
    for (int i = 0; i < nbuttons; i++) {
        int on = i == sel;
        rect(48, by, W - 96, 52, on ? 0x38284a : 0x181c34);
        if (on)
            rect(48, by, 6, 52, 0xffd040);
        text(72, by + 18, button_label(buttons[i]), 16, on ? 0xffd040 : 0xe0e0e8);
        button_rects[i] = (SDL_FRect){48, by, W - 96, 52};
        by += 64;
    }
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ main */
static void find_folders(void)
{
    const char *b = SDL_getenv("TD6_BUNDLE");
    if (b)
        SDL_strlcpy(bundle, b, sizeof bundle);
    else {
#ifdef _WIN32
        SDL_strlcpy(bundle, SDL_GetBasePath(), sizeof bundle);      /* (the zip's folder: launcher and bundle) */
        size_t k = SDL_strlen(bundle);
        if (k && (bundle[k - 1] == '/' || bundle[k - 1] == '\\'))
            bundle[k - 1] = 0;
#else
        SDL_snprintf(bundle, sizeof bundle, "%s../lib/td6-recomp", SDL_GetBasePath());
#endif
    }
    const char *u = SDL_getenv("TD6_USER");
    if (u)
        SDL_strlcpy(user, u, sizeof user);
    else {                              /* (as the game's own) */
#ifdef _WIN32
        SDL_snprintf(user, sizeof user, "%s/td6-native", SDL_getenv("APPDATA"));
#else
        SDL_snprintf(user, sizeof user, "%s/.local/share/td6-native", SDL_getenv("HOME"));
#endif
    }
    for (char *c = user; *c; c++)
        if (*c == '\\')
            *c = '/';
    for (char *c = bundle; *c; c++)
        if (*c == '\\')
            *c = '/';
    make_dirs(user);
    char vp[1024];
    path(vp, sizeof vp, bundle, "VERSION");
    char *v = read_text(vp);
    SDL_strlcpy(version, v ? v : "unknown", sizeof version);
    SDL_free(v);
    for (char *c = version; *c; c++)
        if (*c == '\n' || *c == '\r')
            *c = 0;
    const char *r = SDL_strrchr(version, '|');
    if (r)
        SDL_strlcpy(release, r + 1, sizeof release);
}

int main(int argc, char **argv)
{
    lock = SDL_CreateMutex();
    find_folders();
    int force = 0, no_launch = 0;
    const char *from = NULL;
    for (int i = 1; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--setup"))
            force = 1;
        else if (!SDL_strcmp(argv[i], "--setup-from") && i + 1 < argc)
            from = argv[++i];
        else if (!SDL_strcmp(argv[i], "--no-launch"))
            no_launch = 1;
    }
    if (from) {                          /* in the terminal */
        headless = 1;
        SDL_strlcpy(source, from, sizeof source);
        setup_task(NULL);
        if (!task_ok) {
            SDL_Log("setup failed: %s: %s", status, detail);
            return 1;
        }
        if (!no_launch)
            launch_game(1, argv);
        return 0;
    }
    if (!force && built()) {
        launch_game(argc, argv);
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL: %s", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("Test Drive 6 - Here In My Recomp", 1000, 600,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    ren = SDL_CreateRenderer(win, NULL);
    SDL_SetRenderVSync(ren, 1);
    have_found = detect(found, sizeof found);
    if (!have_found && game_installed() && !force) {   /* (files there already: just build) */
        page = P_WORK;
        start(setup_task);
    }
    int quit = 0;
    /* TD6_LAUNCHER_SHOT=FILE saves each page it shows as FILE-<page>.bmp and quits on the first */
    const char *shot = SDL_getenv("TD6_LAUNCHER_SHOT");
    int shot_page = -1, page_frames = 0;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int act = -1;
            switch (e.type) {
            case SDL_EVENT_QUIT: quit = 1; break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_UP) sel--;
                else if (e.key.key == SDLK_DOWN) sel++;
                else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE) act = sel;
                else if (e.key.key == SDLK_ESCAPE && page != P_WORK) quit = 1;
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) sel--;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) sel++;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) act = sel;
                else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST && page == P_ERROR) page = P_WELCOME;
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                    static int held;
                    int d = e.gaxis.value < -20000 ? -1 : e.gaxis.value > 20000 ? 1 : 0;
                    if (d && !held) sel += d;
                    held = d;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                for (int i = 0; i < nbuttons; i++) {
                    SDL_FRect r = button_rects[i];
                    float x = e.motion.x * SDL_GetWindowPixelDensity(win) / unit,
                          y = e.motion.y * SDL_GetWindowPixelDensity(win) / unit;
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                        sel = i;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                float x = e.button.x * SDL_GetWindowPixelDensity(win) / unit,
                      y = e.button.y * SDL_GetWindowPixelDensity(win) / unit;
                for (int i = 0; i < nbuttons; i++) {
                    SDL_FRect r = button_rects[i];
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                        act = i;
                }
                break;
            }
            }
            if (act >= 0 && act < nbuttons && page != P_WORK) {
                int b = buttons[act];
                if (b == B_QUIT)
                    quit = 1;
                else if (b == B_PLAY) {
                    launch_game(argc, argv);
                    quit = 1;
                } else
                    press(b);
            }
        }
        if (dialog_done) {                /* a file or folder was chosen */
            dialog_done = 0;
            if (dialog_result[0]) {
                SDL_strlcpy(source, dialog_result, sizeof source);
                page = P_WORK;
                start(setup_task);
            }
        }
        if (page == P_WORK && task_done)
            page = task_ok ? P_DONE : P_ERROR, sel = 0;
        draw();
        if (shot) {
            if (page != shot_page)
                shot_page = page, page_frames = 0;
            if (++page_frames == 20) {
                char f[1100];
                SDL_snprintf(f, sizeof f, "%s-%d.bmp", shot, page);
                SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
                if (sf) {
                    SDL_SaveBMP(sf, f);
                    SDL_DestroySurface(sf);
                }
                quit = 1;
            }
        }
    }
    SDL_Quit();
    return 0;
}
