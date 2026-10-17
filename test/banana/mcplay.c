/* mcplay - install and start a Minecraft version from the shell, with the
 * launcher's own code (src/mc.c): what the window does on Play, with its
 * progress and the game's log on the console. For testing in QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <banana.h>
#include "mc.h"

static volatile int g_done;

/* the launcher's status line and the game's log, as they change */
static int watch(void* arg) {
    (void)arg;
    char last[200] = "";
    long logpos = 0;
    while (!g_done) {
        banana_lock(&mc_progress.lock);
        char s[200];
        snprintf(s, sizeof(s), "%s", mc_progress.status);
        int done = mc_progress.done, total = mc_progress.total;
        banana_unlock(&mc_progress.lock);
        if (strcmp(s, last)) { printf("[mcplay] %s (%d/%d)\n", s, done, total); snprintf(last, sizeof(last), "%s", s); }
        FILE* f = fopen(mc_log_path(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            if (n < logpos) logpos = 0;
            if (n > logpos) {
                fseek(f, logpos, SEEK_SET);
                char buf[4096];
                size_t got;
                while (logpos < n && (got = fread(buf, 1, sizeof(buf), f)) > 0) { fwrite(buf, 1, got, stdout); logpos += (long)got; }
                fflush(stdout);
            }
            fclose(f);
        }
        banana_sleep(500);
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* version = NULL;
    const char* name = "BananaTester";
    int ram = 2048;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) setenv("MC_DIR", argv[++i], 1);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--jvm") && i + 1 < argc) setenv("MC_JVM_OPTS", argv[++i], 1);
        else version = argv[i];
    }
    if (!version) { printf("usage: mcplay <version> [--dir <game folder>] [--name <player>] [--ram <MB>]\n"); return 2; }
    char err[300];
    if (mc_init(err, sizeof(err))) { printf("[mcplay] %s\n", err); return 1; }
    char pre[600];
    mc_preflight(pre, sizeof(pre));
    printf("[mcplay] game folder %s, JDK %s, LWJGL %s\n%s", mc_root, mc_java_home() ? mc_java_home() : "-",
           mc_lwjgl_dir() ? mc_lwjgl_dir() : "-", pre);
    if (mc_load_manifest(err, sizeof(err))) printf("[mcplay] version list: %s\n", err);
    int t = banana_thread(watch, NULL);
    int r = mc_play(version, name, ram, err, sizeof(err));
    banana_sleep(1200);                                 /* (the log's last lines) */
    g_done = 1;
    if (t > 0) banana_join(t);
    printf("[mcplay] %s\n", r ? err : "the game ended normally");
    return r ? 1 : 0;
}
