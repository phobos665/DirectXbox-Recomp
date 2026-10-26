/*
 * android_main.c - the title's entry on Android.
 *
 * A title is libmain.so there, loaded by SDL's Java activity, which calls
 * SDL_main on a thread of its own. The title's main.c is not changed: its
 * main() is compiled as recomp_title_main (cmake/android.cmake), and this
 * file gives it what a desktop process would already have.
 *
 *   - Output. An app's stdout and stderr go nowhere, so both are sent down a
 *     pipe to logcat (tag "xboxrecomp") and to xboxrecomp.log in the app's
 *     files folder, which `adb pull` can fetch.
 *   - Switches. An app has no environment to start it with, so the RECOMP_*
 *     switches are read from recomp_env.txt in the same folder, one
 *     KEY=VALUE a line (# starts a comment); `adb push` puts one there.
 *   - Folders. The game in <files>/game and the per-user folder (settings,
 *     saves, caches) in <files>/user, unless the file names others.
 *
 * <files> is the app's own external folder,
 * /storage/emulated/0/Android/data/<package>/files: it needs no permission,
 * adb can reach it, and uninstalling the app deletes it.
 */
#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED   /* SDL_main is defined here, by hand */
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The title's main(), renamed where its main.c is compiled. */
extern int recomp_title_main(int argc, char **argv);

static FILE *g_log_file;

/* Copies the pipe to logcat, a line at a time, and to the log file. */
static void *log_pump(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[4096], line[1024];
    size_t used = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof buf)) > 0) {
        ssize_t i;
        if (g_log_file) {
            fwrite(buf, 1, (size_t)n, g_log_file);
            fflush(g_log_file);
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n' || used == sizeof line - 1) {
                line[used] = '\0';
                __android_log_write(ANDROID_LOG_INFO, "xboxrecomp", line);
                used = 0;
                if (buf[i] == '\n')
                    continue;
            }
            line[used++] = buf[i];
        }
    }
    return NULL;
}

static void start_output(const char *files)
{
    char path[1024];
    int fds[2];
    pthread_t thread;

    snprintf(path, sizeof path, "%s/xboxrecomp.log", files);
    g_log_file = fopen(path, "w");
    if (pipe(fds) != 0)
        return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pthread_create(&thread, NULL, log_pump, (void *)(intptr_t)fds[0]) == 0)
        pthread_detach(thread);
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static void read_switches(const char *files)
{
    char path[1024], line[1024];
    FILE *f;

    snprintf(path, sizeof path, "%s/recomp_env.txt", files);
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq;
        if (!*s || *s == '#' || !(eq = strchr(s, '=')))
            continue;
        *eq = '\0';
        setenv(trim(s), trim(eq + 1), 1);
        fprintf(stderr, "[ANDROID] %s=%s (recomp_env.txt)\n", trim(s), trim(eq + 1));
    }
    fclose(f);
}

static void default_dir(const char *name, const char *files, const char *leaf)
{
    char path[1024];
    const char *v = getenv(name);

    if (v && *v)
        return;
    snprintf(path, sizeof path, "%s/%s", files, leaf);
    mkdir(path, 0770);
    setenv(name, path, 1);
}

__attribute__((visibility("default")))
int SDL_main(int argc, char *argv[])
{
    const char *files;

    SDL_SetMainReady();
    files = SDL_GetAndroidExternalStoragePath();
    if (!files) {
        __android_log_print(ANDROID_LOG_ERROR, "xboxrecomp",
                            "no external files folder: %s", SDL_GetError());
        return 1;
    }
    mkdir(files, 0770);
    start_output(files);
    fprintf(stderr, "[ANDROID] files folder %s\n", files);
    read_switches(files);
    default_dir("RECOMP_GAME_DIR", files, "game");
    default_dir("RECOMP_USER_DIR", files, "user");
    if (chdir(files) != 0)
        fprintf(stderr, "[ANDROID] chdir %s: %s\n", files, strerror(errno));
    return recomp_title_main(argc, argv);
}

#endif /* __ANDROID__ */
