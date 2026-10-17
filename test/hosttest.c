/* hosttest - runs the launcher's logic (src/mc.c) on Linux: the version
 * list, a whole install, and the game started in a desktop JDK's libjvm.so.
 *     test/run-host.sh <version> [skip-assets]
 * Banana OS's calls are done here with curl and pthreads. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "banana.h"
#include "../src/hash.h"
#include "../src/mc.h"

/* like Banana OS (kernel/app.c): one answer is 16 MiB at most */
#define HTTP_MAX (16ul << 20)

/* curl, the body into a malloc'd buffer: the HTTP status, or -1 */
static int curl_get(const char* url, const char* header, char** data, unsigned long* len, char* err, int errcap) {
    char hf[64] = "", cmd[1400];
    snprintf(hf, sizeof(hf), "/tmp/mcl-hdr-%d-%lx", getpid(), (unsigned long)pthread_self());
    snprintf(cmd, sizeof(cmd), "curl -sSL --retry 2 -D '%s' %s%s%s '%s'", hf,
             header ? "-H '" : "", header ? header : "", header ? "'" : "", url);
    FILE* p = popen(cmd, "r");
    if (!p) { snprintf(err, errcap, "popen"); return -1; }
    size_t cap = 1 << 16, n = 0, r;
    char* d = malloc(cap);
    while ((r = fread(d + n, 1, cap - n - 1, p)) > 0) {
        n += r;
        if (n + 1 >= cap) d = realloc(d, cap *= 2);
    }
    int rc = pclose(p);
    int status = -1;
    FILE* h = fopen(hf, "r");
    char line[512];
    while (h && fgets(line, sizeof(line), h)) if (!strncmp(line, "HTTP/", 5)) sscanf(line, "%*s %d", &status);
    if (h) fclose(h);
    remove(hf);
    if (rc != 0 || status < 0) { free(d); snprintf(err, errcap, "%s: curl failed", url); return -1; }
    if (n > HTTP_MAX) { free(d); snprintf(err, errcap, "too big (16 MiB max)"); return -1; }
    d[n] = 0;
    *data = d;
    *len = n;
    return status;
}

int banana_http_get(const char* url, char** data, unsigned long* len, char* err, int errcap) {
    int st = curl_get(url, NULL, data, len, err, errcap);
    if (st < 0) return -1;
    if (st >= 400) { free(*data); snprintf(err, errcap, "HTTP %d", st); return -1; }
    return 0;
}

int banana_http_fetch(const banana_http_req_t* rq, banana_http_resp_t* rs) {
    memset(rs, 0, sizeof(*rs));
    char hdr[256] = "";
    if (rq->headers) { snprintf(hdr, sizeof(hdr), "%s", rq->headers); hdr[strcspn(hdr, "\r\n")] = 0; }
    int st = curl_get(rq->url, hdr[0] ? hdr : NULL, &rs->data, &rs->len, rs->err, sizeof(rs->err));
    if (st < 0) return -1;
    rs->status = st;
    return 0;
}

static pthread_t threads[64];
static int nthreads;
struct tramp { int (*fn)(void*); void* arg; int id; };
static __thread int g_tid;                              /* 0: the main thread */
int banana_thread_id(void) { return g_tid; }
static void* run(void* a) { struct tramp t = *(struct tramp*)a; free(a); g_tid = t.id; return (void*)(long)t.fn(t.arg); }
int banana_thread(int (*fn)(void*), void* arg) {
    struct tramp* t = malloc(sizeof(*t));
    t->fn = fn; t->arg = arg; t->id = nthreads + 1;
    if (nthreads == 64 || pthread_create(&threads[nthreads], NULL, run, t)) return -1;
    return ++nthreads;
}
int banana_join(int id) { void* r; pthread_join(threads[id - 1], &r); return (int)(long)r; }
void banana_lock(banana_mutex_t* m) { while (__sync_lock_test_and_set(m, 1)) sched_yield(); }
void banana_unlock(banana_mutex_t* m) { __sync_lock_release(m); }

static int check(const char* what, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); return ok ? 0 : 1; }

int main(int argc, char** argv) {
    int bad = 0;
    char hx[41], uuid[33], err[300];
    unsigned char h[20];
    sha1("abc", 3, h); hex(h, 20, hx);
    bad += check("sha1(abc)", !strcmp(hx, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    md5("", 0, h); hex(h, 16, hx);
    bad += check("md5()", !strcmp(hx, "d41d8cd98f00b204e9800998ecf8427e"));
    static char big[1000]; memset(big, 'a', sizeof(big));
    sha1(big, sizeof(big), h); hex(h, 20, hx);
    bad += check("sha1(1000 x a)", !strcmp(hx, "291e9a6c66994949b57ba5e650361e98fc36b1ba"));
    /* Notch's offline UUID, as offline-mode servers compute it */
    mc_offline_uuid("Notch", uuid);
    bad += check("offline uuid(Notch)", !strcmp(uuid, "b50ad385829d3141a2167e7d7539ba7f"));
    bad += check("names", mc_valid_name("Steve_12") && !mc_valid_name("ab") && !mc_valid_name("bad name"));
    if (argc < 2) return bad;

    if (mc_init(err, sizeof(err))) { printf("init: %s\n", err); return 1; }
    if (mc_load_manifest(err, sizeof(err))) { printf("manifest: %s\n", err); return 1; }
    bad += check("version list", mc_nversions > 500);
    printf("%s\n", mc_progress.status);
    if (mc_install(argv[1], err, sizeof(err))) { printf("install: %s\n", err); return 1; }
    printf("installed %s: %d files, %lu MB this time\n", argv[1], mc_progress.total, mc_progress.bytes >> 20);
    if (!mc_play(argv[1], "BananaTester", 1024, err, sizeof(err))) printf("game ended normally\n");
    else printf("game: %s\n", err);
    printf("log: %s\n", mc_log_path());
    return bad;
}
