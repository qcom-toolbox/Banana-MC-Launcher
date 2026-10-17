/* mc - Minecraft versions: download and run (see mc.h). */
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <jni.h>
#include "hash.h"
#include "json.h"
#include "mc.h"

#define MANIFEST_URL  "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
#define LIBRARIES_URL "https://libraries.minecraft.net/"
#define RESOURCES_URL "https://resources.download.minecraft.net/"
#define LAUNCHER_NAME "banana-launcher"
#define LAUNCHER_VER  "1.0"
#define WORKERS       6

mc_version*   mc_versions;
int           mc_nversions;
mc_progress_t mc_progress;
char          mc_root[256];
static char   g_log[300];

void mc_status(const char* fmt, ...) {
    char s[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof(s), fmt, ap);
    va_end(ap);
    banana_lock(&mc_progress.lock);
    memcpy(mc_progress.status, s, sizeof(s));
    banana_unlock(&mc_progress.lock);
}

const char* mc_log_path(void) { return g_log; }

/* ── files ─────────────────────────────────────────────────────────── */

static int mkdirs(const char* path) {
    char p[512];
    snprintf(p, sizeof(p), "%s", path);
    for (char* s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        mkdir(p, 0755);
        *s = '/';
    }
    struct stat st;
    return mkdir(p, 0755) == 0 || (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

static int mkparent(const char* path) {
    char p[512];
    snprintf(p, sizeof(p), "%s", path);
    char* s = strrchr(p, '/');
    if (!s || s == p) return 0;
    *s = 0;
    return mkdirs(p);
}

static long file_size(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}

static int write_file(const char* path, const void* data, unsigned long len) {
    if (mkparent(path)) return -1;
    /* (one per thread: the asset index names some files twice, and two
     * download threads may write the same one at once) */
    char tmp[540];
    snprintf(tmp, sizeof(tmp), "%s.part%d", path, banana_thread_id());
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    ok &= fclose(f) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok ? 0 : -1;
}

/* the whole file into a malloc'd buffer (NUL after it) */
static char* read_file(const char* path, unsigned long* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* d = n >= 0 ? (char*)malloc((size_t)n + 1) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (!d) return NULL;
    d[n] = 0;
    if (len) *len = (unsigned long)n;
    return d;
}

static int sha1_ok(const void* data, unsigned long len, const char* want) {
    if (!want || !want[0]) return 1;
    unsigned char h[20];
    char hx[41];
    sha1(data, len, h);
    hex(h, 20, hx);
    return strcmp(hx, want) == 0;
}

static jv* load_json(const char* path, char* err, int ecap) {
    unsigned long n;
    char* d = read_file(path, &n);
    if (!d) { snprintf(err, (size_t)ecap, "cannot read %s", path); return NULL; }
    jv* v = json_parse(d, n, err, ecap);
    free(d);
    return v;
}

/* Banana OS hands an app at most 16 MiB per HTTP answer: bigger files (the
 * game's jar: 23 MB for 1.20.4) come in pieces, with Range requests */
#define CHUNK (8ul << 20)

/* GET url, size bytes, piece by piece into one malloc'd buffer: 0, or -1 */
static int get_ranged(const char* url, unsigned long size, char** data, char* err, int ecap) {
    char* buf = (char*)malloc(size + 1);
    if (!buf) { snprintf(err, (size_t)ecap, "out of memory for %lu MB", size >> 20); return -1; }
    for (unsigned long off = 0; off < size;) {
        unsigned long last = off + CHUNK < size ? off + CHUNK - 1 : size - 1;
        char range[80];
        snprintf(range, sizeof(range), "Range: bytes=%lu-%lu\r\n", off, last);
        banana_http_req_t rq;
        banana_http_resp_t rs;
        memset(&rq, 0, sizeof(rq));
        rq.url = url;
        rq.headers = range;
        rq.timeout_ms = 30000;
        if (banana_http_fetch(&rq, &rs) != 0) { snprintf(err, (size_t)ecap, "%s: %s", url, rs.err); free(buf); return -1; }
        /* 206: the piece; 200: the whole file (a server without ranges) */
        int whole = rs.status == 200 && rs.len == size;
        if (!(rs.status == 206 && rs.len == last - off + 1) && !whole) {
            snprintf(err, (size_t)ecap, "%s: HTTP %d, %lu bytes for bytes %lu-%lu", url, rs.status, rs.len, off, last);
            free(rs.data);
            free(buf);
            return -1;
        }
        memcpy(buf + (whole ? 0 : off), rs.data, rs.len);
        free(rs.data);
        off = whole ? size : last + 1;
        banana_lock(&mc_progress.lock);
        mc_progress.bytes += rs.len;                    /* (progress while the big file comes) */
        banana_unlock(&mc_progress.lock);
    }
    buf[size] = 0;
    *data = buf;
    return 0;
}

/* GET url into path, checked against sha1 (if any); 3 tries. size: what it
 * should be (-1: not known). 0, or -1 */
static int fetch(const char* url, const char* path, const char* want_sha1, long size, char* err, int ecap) {
    int ranged = size > (long)CHUNK;
    for (int attempt = 0; attempt < 3; attempt++) {
        char* data = NULL;
        unsigned long len = 0;
        if (ranged) {
            if (get_ranged(url, (unsigned long)size, &data, err, ecap) != 0) continue;
            len = (unsigned long)size;
        } else if (banana_http_get(url, &data, &len, err, ecap) != 0) {
            continue;
        }
        if (!sha1_ok(data, len, want_sha1)) {
            snprintf(err, (size_t)ecap, "%s: checksum does not match", url);
            free(data);
            continue;
        }
        int r = write_file(path, data, len);
        free(data);
        if (r) { snprintf(err, (size_t)ecap, "cannot write %s", path); return -1; }
        if (!ranged) {
            banana_lock(&mc_progress.lock);
            mc_progress.bytes += len;
            banana_unlock(&mc_progress.lock);
        }
        return 0;
    }
    return -1;
}

/* ── setup ─────────────────────────────────────────────────────────── */

int mc_init(char* err, int ecap) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = "/home/banana";
    const char* dir = getenv("MC_DIR");                 /* (another game folder: test/banana's mcplay) */
    if (dir && dir[0]) snprintf(mc_root, sizeof(mc_root), "%s", dir);
    else snprintf(mc_root, sizeof(mc_root), "%s/.minecraft", home);
    snprintf(g_log, sizeof(g_log), "%s/logs/banana-launcher.log", mc_root);
    static const char* const dirs[] = { "", "/versions", "/libraries", "/assets/indexes", "/assets/objects",
                                        "/logs", "/natives", NULL };
    for (int i = 0; dirs[i]; i++) {
        char p[300];
        snprintf(p, sizeof(p), "%s%s", mc_root, dirs[i]);
        if (mkdirs(p)) { snprintf(err, (size_t)ecap, "cannot make %s", p); return -1; }
    }
    return 0;
}

int mc_java_guess(const mc_version* v) {
    if (v->java) return v->java;
    if (strcmp(v->time, "2024-04-23") >= 0) return 21;      /* 1.20.5 / 24w14a */
    if (strcmp(v->time, "2021-11-30") >= 0) return 17;      /* 1.18 */
    if (strcmp(v->time, "2021-06-08") >= 0) return 16;      /* 1.17 */
    return 8;
}

const char* mc_unsupported(const mc_version* v) {
    static char tag[16];
    int java = mc_java_guess(v);
    if (java > MC_JAVA_MAJOR) { snprintf(tag, sizeof(tag), "Java %d", java); return tag; }
    /* LWJGL 3 came with 17w43a (1.13's snapshots): before it, LWJGL 2, which
     * Banana OS has no port of (and up to 1.5.2, a launchwrapper made for Java 8) */
    if (v->lwjgl2 || strcmp(v->time, "2017-10-25") < 0) return "LWJGL 2";
    return NULL;
}

/* old versions Banana OS cannot run, by their json: why (NULL: none) */
static const char* old_version(const jv* json) {
    const char* main = jstr(json, "mainClass");
    if (main && !strcmp(main, "net.minecraft.launchwrapper.Launch"))
        return "starts through launchwrapper, which only runs on Java 8";
    jv* libs = jget(json, "libraries");
    for (int i = 0; i < jlen(libs); i++) {
        const char* n = jstr(jat(libs, i), "name");
        if (n && !strncmp(n, "org.lwjgl.lwjgl:", 16)) return "uses LWJGL 2, and Banana OS only has LWJGL 3";
    }
    return NULL;
}

int mc_load_manifest(char* err, int ecap) {
    char path[300];
    snprintf(path, sizeof(path), "%s/versions/version_manifest_v2.json", mc_root);
    mc_status("Getting the version list...");
    char nerr[160] = "";
    int online = fetch(MANIFEST_URL, path, NULL, -1, nerr, sizeof(nerr)) == 0;
    jv* m = load_json(path, err, ecap);
    if (!m) {
        if (!online) snprintf(err, (size_t)ecap, "no version list (offline?): %s", nerr);
        return -1;
    }
    jv* vs = jget(m, "versions");
    int n = jlen(vs);
    mc_version* out = (mc_version*)calloc((size_t)(n ? n : 1), sizeof(mc_version));
    if (!out) { json_free(m); snprintf(err, (size_t)ecap, "out of memory"); return -1; }
    int k = 0;
    for (int i = 0; i < n; i++) {
        jv* v = jat(vs, i);
        const char *id = jstr(v, "id"), *url = jstr(v, "url");
        if (!id || !url) continue;
        mc_version* o = &out[k++];
        snprintf(o->id, sizeof(o->id), "%s", id);
        snprintf(o->url, sizeof(o->url), "%s", url);
        snprintf(o->type, sizeof(o->type), "%s", jstr(v, "type") ? jstr(v, "type") : "");
        snprintf(o->sha1, sizeof(o->sha1), "%s", jstr(v, "sha1") ? jstr(v, "sha1") : "");
        snprintf(o->time, sizeof(o->time), "%s", jstr(v, "releaseTime") ? jstr(v, "releaseTime") : "");
    }
    json_free(m);
    free(mc_versions);
    mc_versions = out;
    mc_nversions = k;
    mc_status(online ? "%d versions" : "%d versions (offline: the saved list)", k);
    return 0;
}

static const mc_version* find_version(const char* id) {
    for (int i = 0; i < mc_nversions; i++) if (!strcmp(mc_versions[i].id, id)) return &mc_versions[i];
    return NULL;
}

/* ── the JDK ───────────────────────────────────────────────────────── */

static int has_jvm(const char* home) {
    char p[512];
    snprintf(p, sizeof(p), "%s/lib/server/libjvm.so", home);
    return file_size(p) > 0;
}

const char* mc_java_home(void) {
    static char found[300];
    const char* env = getenv("JAVA_HOME");
    if (env && has_jvm(env)) return env;
    static const char* const homes[] = { "/usr/java/jdk", "/jdk", "/mnt/jdk", "/home/banana/jdk", "/mnt/usb/jdk", "/mnt/virtio/jdk",
                                         "/mnt/sata/jdk", "/mnt/nvme/jdk", NULL };
    for (int i = 0; homes[i]; i++) if (has_jvm(homes[i])) return homes[i];
    snprintf(found, sizeof(found), "%s/jdk", mc_root);
    return has_jvm(found) ? found : NULL;
}

/* the folder next to a JDK at <dir>/jdk: <dir>/<name> (the java package of
 * Banana OS's releases installs /usr/java: jdk/, lwjgl/, lib/libOSMesa.so) */
static void beside_jdk(const char* jh, const char* name, char* out, int cap) {
    char d[300];
    snprintf(d, sizeof(d), "%s", jh);
    char* s = strrchr(d, '/');
    if (s && s != d) *s = 0;
    else snprintf(d, sizeof(d), "%s", "");
    snprintf(out, (size_t)cap, "%s/%s", d, name);
}

static int has_lwjgl(const char* dir) {
    char p[400];
    snprintf(p, sizeof(p), "%s/liblwjgl.so", dir);
    return file_size(p) > 0;
}

/* LWJGL 3 for Banana OS (ports/lwjgl): its native libraries and its jars.
 * ~/.minecraft/natives, the lwjgl folder beside the JDK (/usr/java/lwjgl), /lwjgl, /mnt/<disk>/lwjgl */
const char* mc_lwjgl_dir(void) {
    static char found[300];
    const char* env = getenv("LWJGL_HOME");
    if (env && has_lwjgl(env)) return env;
    snprintf(found, sizeof(found), "%s/natives", mc_root);
    if (has_lwjgl(found)) return found;
    const char* jh = mc_java_home();
    if (jh) {
        beside_jdk(jh, "lwjgl", found, sizeof(found));
        if (has_lwjgl(found)) return found;
    }
    static const char* const dirs[] = { "/usr/java/lwjgl", "/lwjgl", "/mnt/usb/lwjgl", "/mnt/virtio/lwjgl", "/mnt/sata/lwjgl",
                                        "/mnt/nvme/lwjgl", NULL };
    for (int i = 0; dirs[i]; i++) if (has_lwjgl(dirs[i])) return dirs[i];
    return NULL;
}

void mc_preflight(char* out, int cap) {
    out[0] = 0;
    int n = 0;
#define NOTE(...) do { if (n < cap) n += snprintf(out + n, (size_t)(cap - n), __VA_ARGS__); } while (0)
    if (sizeof(void*) != 8) NOTE("Java needs the 64-bit Banana OS.\n");
    const char* jh = mc_java_home();
    if (!jh) {
        NOTE("No Java: install java.bpk from the Banana OS release (pkg install java.bpk) - it goes to /usr/java.\n");
    } else {
        char p[300];
        snprintf(p, sizeof(p), "%s/release", jh);
        char* rel = read_file(p, NULL);
        const char* mods = rel ? strstr(rel, "MODULES=") : NULL;
        static const char* const need[] = { "java.logging", "java.desktop", "jdk.zipfs", "java.scripting", "java.sql", NULL };
        for (int i = 0; mods && need[i]; i++)
            if (!strstr(mods, need[i])) {
                NOTE("This JDK lacks %s, which Minecraft needs: install a newer java.bpk.\n", need[i]);
                break;
            }
        free(rel);
    }
    if (!mc_lwjgl_dir())
        NOTE("No LWJGL for Banana OS (/usr/java/lwjgl, from java.bpk): the game stops when it opens its window.\n");
#undef NOTE
}

/* ── names ─────────────────────────────────────────────────────────── */

int mc_valid_name(const char* s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return n >= 3 && n <= 16;
}

void mc_offline_uuid(const char* name, char out[33]) {
    /* what the server does in offline mode: UUID v3 of "OfflinePlayer:<name>" */
    char s[64];
    snprintf(s, sizeof(s), "OfflinePlayer:%s", name);
    unsigned char h[16];
    md5(s, strlen(s), h);
    h[6] = (unsigned char)((h[6] & 0x0f) | 0x30);
    h[8] = (unsigned char)((h[8] & 0x3f) | 0x80);
    hex(h, 16, out);
}

/* ── a version's plan: what to download, the class path ───────────── */

typedef struct {
    char* url;
    char* path;
    char  sha1[41];
    long  size;                          /* -1: not known */
} job_t;

typedef struct {
    jv*    json;
    job_t* jobs;
    int    njobs, capjobs;
    char*  classpath;                    /* ':'-separated */
    size_t cplen, cpcap;
    char   assets_id[64];
    int    assets_virtual, assets_resources;
    char   log_config[300];
    char   log_arg[200];
} plan_t;

static int add_job(plan_t* p, const char* url, const char* path, const char* sha1v, long size) {
    if (p->njobs == p->capjobs) {
        int nc = p->capjobs ? p->capjobs * 2 : 256;
        job_t* nj = (job_t*)realloc(p->jobs, (size_t)nc * sizeof(job_t));
        if (!nj) return -1;
        p->jobs = nj;
        p->capjobs = nc;
    }
    job_t* j = &p->jobs[p->njobs++];
    j->url = strdup(url);
    j->path = strdup(path);
    snprintf(j->sha1, sizeof(j->sha1), "%s", sha1v ? sha1v : "");
    j->size = size;
    return j->url && j->path ? 0 : -1;
}

static int add_cp(plan_t* p, const char* path) {
    size_t n = strlen(path);
    /* the same jar twice: once is enough */
    for (const char* s = p->classpath; s && (s = strstr(s, path)); s += n)
        if ((s == p->classpath || s[-1] == ':') && (s[n] == ':' || s[n] == 0)) return 0;
    if (p->cplen + n + 2 > p->cpcap) {
        size_t nc = (p->cpcap ? p->cpcap * 2 : 8192) + n;
        char* c = (char*)realloc(p->classpath, nc);
        if (!c) return -1;
        p->classpath = c;
        p->cpcap = nc;
    }
    if (p->cplen) p->classpath[p->cplen++] = ':';
    memcpy(p->classpath + p->cplen, path, n + 1);
    p->cplen += n;
    return 0;
}

static void plan_free(plan_t* p) {
    for (int i = 0; i < p->njobs; i++) { free(p->jobs[i].url); free(p->jobs[i].path); }
    free(p->jobs);
    free(p->classpath);
    json_free(p->json);
    memset(p, 0, sizeof(*p));
}

/* a library's / argument's "rules": allowed here (Linux-like, 64-bit, no features)? */
static int rules_allow(const jv* rules) {
    if (!rules) return 1;
    int allow = 0;
    for (int i = 0; i < jlen(rules); i++) {
        jv* r = jat(rules, i);
        int match = 1;
        jv* os = jget(r, "os");
        if (os) {
            const char* name = jstr(os, "name");
            const char* arch = jstr(os, "arch");
            if (name && strcmp(name, "linux")) match = 0;
            if (arch && strcmp(arch, sizeof(void*) == 8 ? "x86_64" : "x86")) match = 0;
        }
        if (jget(r, "features")) match = 0;              /* demo, custom resolution, quick play... */
        if (match) allow = jstr(r, "action") && !strcmp(jstr(r, "action"), "allow");
    }
    return allow;
}

/* group:artifact:version[:classifier][@ext] -> group/path/artifact/version/artifact-version[-classifier].ext */
static int maven_path(const char* name, char* out, int cap, char* classifier, int ccap) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", name);
    const char* ext = "jar";
    char* at = strchr(buf, '@');
    if (at) { *at = 0; ext = at + 1; }
    char* part[4] = { 0 };
    int n = 0;
    for (char* s = buf; n < 4; n++) {
        part[n] = s;
        char* c = strchr(s, ':');
        if (!c) { n++; break; }
        *c = 0;
        s = c + 1;
    }
    if (n < 3) return -1;
    for (char* s = part[0]; *s; s++) if (*s == '.') *s = '/';
    snprintf(classifier, (size_t)ccap, "%s", n > 3 ? part[3] : "");
    if (n > 3) snprintf(out, (size_t)cap, "%s/%s/%s/%s-%s-%s.%s", part[0], part[1], part[2], part[1], part[2], part[3], ext);
    else       snprintf(out, (size_t)cap, "%s/%s/%s/%s-%s.%s", part[0], part[1], part[2], part[1], part[2], ext);
    return 0;
}

static int plan_libraries(plan_t* p, char* err, int ecap) {
    jv* libs = jget(p->json, "libraries");
    for (int i = 0; i < jlen(libs); i++) {
        jv* lib = jat(libs, i);
        const char* name = jstr(lib, "name");
        if (!name || !rules_allow(jget(lib, "rules"))) continue;
        char rel[300], cls[64], path[600];
        if (maven_path(name, rel, sizeof(rel), cls, sizeof(cls))) continue;
        /* native code built for Linux (glibc): no use here - Banana OS's own
         * builds go in ~/.minecraft/natives */
        if (!strncmp(cls, "natives-", 8)) continue;
        /* LWJGL modules Banana OS has built: its jars, which carry the
         * checksums of its native libraries (ports/lwjgl/build.sh) */
        if (!strncmp(name, "org.lwjgl:", 10) && !cls[0] && mc_lwjgl_dir()) {
            char art[64], port[400];
            snprintf(art, sizeof(art), "%s", name + 10);
            art[strcspn(art, ":")] = 0;
            snprintf(port, sizeof(port), "%s/%s.jar", mc_lwjgl_dir(), art);
            if (file_size(port) > 0) {
                if (add_cp(p, port)) goto oom;
                continue;
            }
        }
        jv* dl = jget(lib, "downloads");
        jv* art = jget(dl, "artifact");
        if (dl && !art) continue;                       /* old-style natives only (classifiers) */
        if (art) {
            const char* ap = jstr(art, "path");
            const char* url = jstr(art, "url");
            if (ap) snprintf(rel, sizeof(rel), "%s", ap);
            snprintf(path, sizeof(path), "%s/libraries/%s", mc_root, rel);
            if (url && url[0] && add_job(p, url, path, jstr(art, "sha1"), (long)jnum(art, "size", -1))) goto oom;
        } else {
            /* Fabric / Forge style: a maven repository and a name */
            const char* base = jstr(lib, "url");
            if (!base) base = LIBRARIES_URL;
            char url[700];
            snprintf(url, sizeof(url), "%s%s%s", base, base[strlen(base) - 1] == '/' ? "" : "/", rel);
            snprintf(path, sizeof(path), "%s/libraries/%s", mc_root, rel);
            if (add_job(p, url, path, jstr(lib, "sha1"), -1)) goto oom;
        }
        if (add_cp(p, path)) goto oom;
    }
    return 0;
oom:
    snprintf(err, (size_t)ecap, "out of memory");
    return -1;
}

static int plan_assets(plan_t* p, char* err, int ecap) {
    jv* ai = jget(p->json, "assetIndex");
    const char* id = jstr(ai, "id");
    const char* url = jstr(ai, "url");
    if (!id || !url) { snprintf(p->assets_id, sizeof(p->assets_id), "legacy"); return 0; }
    snprintf(p->assets_id, sizeof(p->assets_id), "%s", id);
    char path[400];
    snprintf(path, sizeof(path), "%s/assets/indexes/%s.json", mc_root, id);
    const char* want = jstr(ai, "sha1");
    long have = file_size(path);
    if (have < 0 || (jnum(ai, "size", -1) >= 0 && have != (long)jnum(ai, "size", -1))) {
        mc_status("Getting the asset index %s...", id);
        if (fetch(url, path, want, (long)jnum(ai, "size", -1), err, ecap)) return -1;
    }
    jv* idx = load_json(path, err, ecap);
    if (!idx) return -1;
    p->assets_virtual = jbool(idx, "virtual", 0);
    p->assets_resources = jbool(idx, "map_to_resources", 0);
    jv* objs = jget(idx, "objects");
#ifdef MCL_TEST_NO_OBJECTS
    objs = NULL;                                /* (test/run-host.sh ... skip) */
#endif
    for (int i = 0; i < jlen(objs); i++) {
        jv* o = jat(objs, i);
        const char* h = jstr(o, "hash");
        if (!h || strlen(h) != 40) continue;
        char u[200], f[400];
        snprintf(u, sizeof(u), RESOURCES_URL "%.2s/%s", h, h);
        snprintf(f, sizeof(f), "%s/assets/objects/%.2s/%s", mc_root, h, h);
        if (add_job(p, u, f, h, (long)jnum(o, "size", -1))) { json_free(idx); snprintf(err, (size_t)ecap, "out of memory"); return -1; }
    }
    json_free(idx);
    return 0;
}

/* the version's json (downloaded if needed), what it needs, the class path */
static int plan_make(plan_t* p, const char* id, char* err, int ecap) {
    memset(p, 0, sizeof(*p));
    char vdir[300], path[400];
    snprintf(vdir, sizeof(vdir), "%s/versions/%s", mc_root, id);
    snprintf(path, sizeof(path), "%s/%s.json", vdir, id);
    const mc_version* v = find_version(id);
    if (v) {
        unsigned long n;
        char* d = read_file(path, &n);
        int fresh = d && sha1_ok(d, n, v->sha1);
        free(d);
        if (!fresh) {
            mc_status("Getting %s...", id);
            char ferr[200];
            if (fetch(v->url, path, v->sha1, -1, ferr, sizeof(ferr)) && file_size(path) <= 0) {
                snprintf(err, (size_t)ecap, "%s", ferr);
                return -1;
            }
        }
    }
    if (!(p->json = load_json(path, err, ecap))) return -1;
    if (jget(p->json, "inheritsFrom")) {
        snprintf(err, (size_t)ecap, "%s builds on another version (inheritsFrom): not supported yet", id);
        return -1;
    }
    int java = (int)jnum(jget(p->json, "javaVersion"), "majorVersion", 8);
    if (v) ((mc_version*)v)->java = java;
    if (java > MC_JAVA_MAJOR) {
        snprintf(err, (size_t)ecap, "%s needs Java %d; Banana OS has Java %d (play 1.20.4 or older)", id, java, MC_JAVA_MAJOR);
        return -1;
    }
    const char* why = old_version(p->json);
    if (why) {
        if (v) ((mc_version*)v)->lwjgl2 = 1;
        snprintf(err, (size_t)ecap, "%s %s: play 1.13 to 1.20.4", id, why);
        return -1;
    }
    if (plan_libraries(p, err, ecap)) return -1;
    /* the game itself: last on the class path, like the official launcher */
    jv* client = jget(jget(p->json, "downloads"), "client");
    snprintf(path, sizeof(path), "%s/%s.jar", vdir, id);
    if (client && jstr(client, "url") &&
        add_job(p, jstr(client, "url"), path, jstr(client, "sha1"), (long)jnum(client, "size", -1))) goto oom;
    if (add_cp(p, path)) goto oom;
    /* log4j's settings (they also fix the Log4Shell hole on old versions) */
    jv* lc = jget(jget(p->json, "logging"), "client");
    jv* lf = jget(lc, "file");
    const char* lid = jstr(lf, "id");
    const char* larg = jstr(lc, "argument");
    if (lid && jstr(lf, "url") && larg && !strchr(lid, '/')) {
        snprintf(p->log_config, sizeof(p->log_config), "%s/assets/log_configs/%s", mc_root, lid);
        snprintf(p->log_arg, sizeof(p->log_arg), "%s", larg);
        if (add_job(p, jstr(lf, "url"), p->log_config, jstr(lf, "sha1"), (long)jnum(lf, "size", -1))) goto oom;
    }
    return plan_assets(p, err, ecap);
oom:
    snprintf(err, (size_t)ecap, "out of memory");
    return -1;
}

/* ── downloading ───────────────────────────────────────────────────── */

typedef struct {
    plan_t*        plan;
    volatile int   next;
    banana_mutex_t lock;
    char           err[200];
} pool_t;

static int have_job(const job_t* j) {
    long n = file_size(j->path);
    return j->size >= 0 ? n == j->size : n > 0;
}

static int worker(void* arg) {
    pool_t* pl = (pool_t*)arg;
    for (;;) {
        banana_lock(&pl->lock);
        int i = pl->next++;
        banana_unlock(&pl->lock);
        if (i >= pl->plan->njobs) break;
        job_t* j = &pl->plan->jobs[i];
        int bad = 0;
        if (!have_job(j)) {
            char e[200];
            if (fetch(j->url, j->path, j->sha1, j->size, e, sizeof(e))) {
                bad = 1;
                banana_lock(&pl->lock);
                if (!pl->err[0]) snprintf(pl->err, sizeof(pl->err), "%s", e);
                banana_unlock(&pl->lock);
            }
        }
        banana_lock(&mc_progress.lock);
        mc_progress.done++;
        mc_progress.failed += bad;
        banana_unlock(&mc_progress.lock);
    }
    return 0;
}

static int download_all(plan_t* p, char* err, int ecap) {
    static pool_t pl;
    memset(&pl, 0, sizeof(pl));
    pl.plan = p;
    banana_lock(&mc_progress.lock);
    mc_progress.done = mc_progress.failed = 0;
    mc_progress.bytes = 0;
    mc_progress.total = p->njobs;
    banana_unlock(&mc_progress.lock);
    int missing = 0;
    for (int i = 0; i < p->njobs; i++) missing += !have_job(&p->jobs[i]);
    mc_status(missing ? "Downloading %d files..." : "Checking %d files...", missing ? missing : p->njobs);
    int ids[WORKERS], n = 0;
    for (int i = 0; i < WORKERS && missing; i++) {
        int t = banana_thread(worker, &pl);
        if (t > 0) ids[n++] = t;
    }
    if (n == 0) worker(&pl);                    /* no threads (or nothing to get): here */
    for (int i = 0; i < n; i++) banana_join(ids[i]);
    if (mc_progress.failed) {
        snprintf(err, (size_t)ecap, "%d files did not download: %s", mc_progress.failed, pl.err);
        return -1;
    }
    return 0;
}

/* old versions read their sounds by name: copy them out of objects/ */
static int unpack_legacy_assets(plan_t* p, char* err, int ecap) {
    if (!p->assets_virtual && !p->assets_resources) return 0;
    char path[400];
    snprintf(path, sizeof(path), "%s/assets/indexes/%s.json", mc_root, p->assets_id);
    jv* idx = load_json(path, err, ecap);
    if (!idx) return -1;
    jv* objs = jget(idx, "objects");
    mc_status("Unpacking the old-style assets...");
    for (int i = 0; i < jlen(objs); i++) {
        const char* name = idx && objs->keys ? objs->keys[i] : NULL;
        const char* h = jstr(jat(objs, i), "hash");
        if (!name || !h || strstr(name, "..")) continue;
        char src[400], dst[600];
        snprintf(src, sizeof(src), "%s/assets/objects/%.2s/%s", mc_root, h, h);
        if (p->assets_resources) snprintf(dst, sizeof(dst), "%s/resources/%s", mc_root, name);
        else                     snprintf(dst, sizeof(dst), "%s/assets/virtual/%s/%s", mc_root, p->assets_id, name);
        if (file_size(dst) == file_size(src)) continue;
        unsigned long n;
        char* d = read_file(src, &n);
        if (d) write_file(dst, d, n);
        free(d);
    }
    json_free(idx);
    return 0;
}

int mc_install(const char* id, char* err, int ecap) {
    plan_t p;
    int r = plan_make(&p, id, err, ecap);
    if (!r) r = download_all(&p, err, ecap);
    if (!r) r = unpack_legacy_assets(&p, err, ecap);
    plan_free(&p);
    if (!r) mc_status("%s is ready", id);
    return r;
}

/* ── arguments ─────────────────────────────────────────────────────── */

typedef struct { const char* key; const char* val; } var_t;

/* s with every ${key} replaced: malloc'd */
static char* subst(const char* s, const var_t* vars) {
    size_t cap = strlen(s) + 64, n = 0;
    char* o = (char*)malloc(cap);
    if (!o) return NULL;
    while (*s) {
        const char* val = NULL;
        size_t skip = 0;
        if (s[0] == '$' && s[1] == '{') {
            const char* e = strchr(s, '}');
            if (e) {
                for (const var_t* v = vars; v->key; v++)
                    if (strlen(v->key) == (size_t)(e - s - 2) && !strncmp(v->key, s + 2, (size_t)(e - s - 2))) { val = v->val; break; }
                if (val) skip = (size_t)(e - s) + 1;
            }
        }
        size_t add = val ? strlen(val) : 1;
        if (n + add + 1 > cap) {
            cap = (n + add + 1) * 2;
            char* no = (char*)realloc(o, cap);
            if (!no) { free(o); return NULL; }
            o = no;
        }
        if (val) { memcpy(o + n, val, add); n += add; s += skip; }
        else o[n++] = *s++;
    }
    o[n] = 0;
    return o;
}

typedef struct { char** v; int n, cap; } strv;

static void sv_add(strv* a, char* s) {
    if (!s) return;
    if (a->n == a->cap) {
        int nc = a->cap ? a->cap * 2 : 32;
        char** nv = (char**)realloc(a->v, (size_t)nc * sizeof(char*));
        if (!nv) { free(s); return; }
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n++] = s;
}

static void sv_free(strv* a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof(*a));
}

/* "arguments": {"game"/"jvm": [ "str", {"rules": [...], "value": "str" | [...]} ]} */
static void add_args(strv* out, const jv* list, const var_t* vars) {
    for (int i = 0; i < jlen(list); i++) {
        jv* a = jat(list, i);
        if (a->t == J_STR) { sv_add(out, subst(a->s, vars)); continue; }
        if (a->t != J_OBJ || !rules_allow(jget(a, "rules"))) continue;
        jv* val = jget(a, "value");
        if (val && val->t == J_STR) sv_add(out, subst(val->s, vars));
        for (int k = 0; val && val->t == J_ARR && k < val->len; k++)
            if (val->items[k]->t == J_STR) sv_add(out, subst(val->items[k]->s, vars));
    }
}

/* ── running it ────────────────────────────────────────────────────── */

typedef jint (JNICALL* create_vm_t)(JavaVM**, void**, void*);

typedef struct {
    const char* java_home;
    strv        jvm, game;
    char        main_class[200];
    char*       err;
    int         ecap;
    int         result;
} run_t;

static void log_line(FILE* f, const char* what, const strv* a) {
    fprintf(f, "%s:\n", what);
    for (int i = 0; i < a->n; i++) fprintf(f, "  %s\n", a->v[i]);
}

/* System.out and System.err to the log file */
static void redirect_output(JNIEnv* env) {
    jclass fos = (*env)->FindClass(env, "java/io/FileOutputStream");
    jclass ps = (*env)->FindClass(env, "java/io/PrintStream");
    jclass sys = (*env)->FindClass(env, "java/lang/System");
    if (!fos || !ps || !sys) { (*env)->ExceptionClear(env); return; }
    jobject f = (*env)->NewObject(env, fos, (*env)->GetMethodID(env, fos, "<init>", "(Ljava/lang/String;Z)V"),
                                  (*env)->NewStringUTF(env, g_log), JNI_TRUE);
    if (!f) { (*env)->ExceptionClear(env); return; }
    jobject out = (*env)->NewObject(env, ps, (*env)->GetMethodID(env, ps, "<init>", "(Ljava/io/OutputStream;Z)V"), f, JNI_TRUE);
    if (!out) { (*env)->ExceptionClear(env); return; }
    (*env)->CallStaticVoidMethod(env, sys, (*env)->GetStaticMethodID(env, sys, "setOut", "(Ljava/io/PrintStream;)V"), out);
    (*env)->CallStaticVoidMethod(env, sys, (*env)->GetStaticMethodID(env, sys, "setErr", "(Ljava/io/PrintStream;)V"), out);
    (*env)->ExceptionClear(env);
}

static void* run_jvm(void* arg) {
    run_t* r = (run_t*)arg;
    r->result = -1;
    char lib[400];
    snprintf(lib, sizeof(lib), "%s/lib/server/libjvm.so", r->java_home);
    void* h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { snprintf(r->err, (size_t)r->ecap, "cannot load the JVM: %s", dlerror()); return NULL; }
    create_vm_t create = (create_vm_t)dlsym(h, "JNI_CreateJavaVM");
    if (!create) { snprintf(r->err, (size_t)r->ecap, "%s has no JNI_CreateJavaVM", lib); return NULL; }

    JavaVMOption* opts = (JavaVMOption*)calloc((size_t)r->jvm.n + 1, sizeof(JavaVMOption));
    if (!opts) { snprintf(r->err, (size_t)r->ecap, "out of memory"); return NULL; }
    for (int i = 0; i < r->jvm.n; i++) opts[i].optionString = r->jvm.v[i];
    JavaVMInitArgs args;
    args.version = JNI_VERSION_10;
    args.options = opts;
    args.nOptions = r->jvm.n;
    args.ignoreUnrecognized = JNI_TRUE;          /* the version's own -XX options this VM may not know */
    JavaVM* vm;
    JNIEnv* env;
    mc_status("Starting Java...");
    if (create(&vm, (void**)&env, &args) != JNI_OK) {
        free(opts);
        snprintf(r->err, (size_t)r->ecap, "the JVM could not start (see %s)", g_log);
        return NULL;
    }
    redirect_output(env);
    char name[200];
    snprintf(name, sizeof(name), "%s", r->main_class);
    for (char* c = name; *c; c++) if (*c == '.') *c = '/';
    jclass cls = (*env)->FindClass(env, name);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "main", "([Ljava/lang/String;)V") : NULL;
    if (!m) {
        (*env)->ExceptionDescribe(env);
        snprintf(r->err, (size_t)r->ecap, "cannot load %s (see the log)", r->main_class);
    } else {
        jobjectArray ja = (*env)->NewObjectArray(env, r->game.n, (*env)->FindClass(env, "java/lang/String"), NULL);
        for (int i = 0; ja && i < r->game.n; i++)
            (*env)->SetObjectArrayElement(env, ja, i, (*env)->NewStringUTF(env, r->game.v[i]));
        mc_status("Minecraft is running");
        (*env)->CallStaticVoidMethod(env, cls, m, ja);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            snprintf(r->err, (size_t)r->ecap, "Minecraft stopped with an error (see the log)");
        } else {
            r->result = 0;
        }
    }
    (*vm)->DestroyJavaVM(vm);                    /* waits for the game's own threads */
    free(opts);
    return NULL;
}

static int jvm_started;

int mc_play(const char* id, const char* user, int ram_mb, char* err, int ecap) {
    if (jvm_started) { snprintf(err, (size_t)ecap, "Java runs once per launcher: close the launcher and open it again"); return -1; }
    if (!mc_valid_name(user)) { snprintf(err, (size_t)ecap, "the name must be 3-16 letters, digits or _"); return -1; }
    const char* jh = mc_java_home();
    if (!jh) { snprintf(err, (size_t)ecap, "no Java: install java.bpk (pkg install java.bpk) - it goes to /usr/java"); return -1; }
    if (sizeof(void*) != 8) { snprintf(err, (size_t)ecap, "Java needs the 64-bit Banana OS"); return -1; }

    plan_t p;
    if (plan_make(&p, id, err, ecap) || download_all(&p, err, ecap) || unpack_legacy_assets(&p, err, ecap)) {
        plan_free(&p);
        return -1;
    }
    const char* main_class = jstr(p.json, "mainClass");
    if (!main_class) { plan_free(&p); snprintf(err, (size_t)ecap, "%s has no mainClass", id); return -1; }

    char uuid[33], natives[300], assets[300], game_assets[400], libdir[300], resources[300];
    mc_offline_uuid(user, uuid);
    /* natives_directory: where the game's libraries unpack what they extract
     * (JNA, netty: jna.tmpdir...) - ~/.minecraft/natives, writable. LWJGL
     * itself loads from its folder (the -D options further down) */
    const char* lwjgl = mc_lwjgl_dir();
    snprintf(natives, sizeof(natives), "%s/natives", mc_root);
    /* OpenGL 3.3 (Mesa's libOSMesa.so) beside the JDK: libglfw.so finds
     * /lib and /mnt/<disk>/lib itself, BGL3_LIBRARY anywhere else */
    char osmesa[300];
    beside_jdk(jh, "lib/libOSMesa.so", osmesa, sizeof(osmesa));
    if (!getenv("BGL3_LIBRARY") && file_size(osmesa) > 0) setenv("BGL3_LIBRARY", osmesa, 1);
    snprintf(assets, sizeof(assets), "%s/assets", mc_root);
    snprintf(libdir, sizeof(libdir), "%s/libraries", mc_root);
    snprintf(resources, sizeof(resources), "%s/resources", mc_root);
    if (p.assets_resources) snprintf(game_assets, sizeof(game_assets), "%s", resources);
    else if (p.assets_virtual) snprintf(game_assets, sizeof(game_assets), "%s/virtual/%s", assets, p.assets_id);
    else snprintf(game_assets, sizeof(game_assets), "%s", assets);
    const char* vtype = jstr(p.json, "type") ? jstr(p.json, "type") : "release";
    var_t vars[] = {
        { "auth_player_name", user },          { "version_name", id },
        { "game_directory", mc_root },         { "assets_root", assets },
        { "game_assets", game_assets },        { "assets_index_name", p.assets_id },
        { "auth_uuid", uuid },                 { "auth_access_token", "0" },
        { "auth_session", "0" },               { "clientid", "0" },
        { "auth_xuid", "0" },                  { "user_type", "legacy" },
        { "version_type", vtype },             { "user_properties", "{}" },
        { "natives_directory", natives },      { "launcher_name", LAUNCHER_NAME },
        { "launcher_version", LAUNCHER_VER },  { "classpath", p.classpath },
        { "classpath_separator", ":" },        { "library_directory", libdir },
        { "path", p.log_config },
        { NULL, NULL },
    };

    static run_t r;
    memset(&r, 0, sizeof(r));
    r.java_home = jh;
    r.err = err;
    r.ecap = ecap;
    snprintf(r.main_class, sizeof(r.main_class), "%s", main_class);
    char s[400];
    snprintf(s, sizeof(s), "-Xmx%dM", ram_mb);
    sv_add(&r.jvm, strdup(s));
    sv_add(&r.jvm, strdup("-Xss2M"));
    /* "arguments.jvm" (1.13+) or the old defaults; -cp is ours to set */
    strv vj = { 0 };
    jv* args = jget(p.json, "arguments");
    if (jget(args, "jvm")) add_args(&vj, jget(args, "jvm"), vars);
    for (int i = 0; i < vj.n; i++) {
        if (!strcmp(vj.v[i], "-cp") || !strcmp(vj.v[i], "-classpath")) { i++; continue; }
        if (vj.v[i][0] == '-' && (vj.v[i][1] == 'D' || vj.v[i][1] == 'X')) { sv_add(&r.jvm, vj.v[i]); vj.v[i] = NULL; }
    }
    sv_free(&vj);
    const char* libs = lwjgl ? lwjgl : natives;
    snprintf(s, sizeof(s), "-Djava.library.path=%s", libs);                 sv_add(&r.jvm, strdup(s));
    snprintf(s, sizeof(s), "-Dorg.lwjgl.librarypath=%s", libs);             sv_add(&r.jvm, strdup(s));
    sv_add(&r.jvm, strdup("-Dminecraft.launcher.brand=" LAUNCHER_NAME));
    sv_add(&r.jvm, strdup("-Dminecraft.launcher.version=" LAUNCHER_VER));
    sv_add(&r.jvm, strdup("-Dfile.encoding=UTF-8"));
    /* LWJGL knows Linux, macOS, Windows and FreeBSD - not "Banana OS" -
     * and Banana OS's LWJGL is built as a Linux one */
    sv_add(&r.jvm, strdup("-Dos.name=Linux"));
    /* jemalloc has no Banana OS build: LWJGL's malloc is the C library's */
    sv_add(&r.jvm, strdup("-Dorg.lwjgl.system.allocator=system"));
    sv_add(&r.jvm, strdup("-Dsun.java.launcher=SUN_STANDARD"));
    snprintf(s, sizeof(s), "-Dsun.java.command=%s", main_class);             sv_add(&r.jvm, strdup(s));
    if (p.log_arg[0]) sv_add(&r.jvm, subst(p.log_arg, vars));
    /* more JVM options, for looking into problems: MC_JVM_OPTS="-Dx=y -Xlog:..." */
    const char* extra = getenv("MC_JVM_OPTS");
    if (extra && extra[0]) {
        char* all = strdup(extra);
        char* sp = NULL;
        for (char* t = all ? strtok_r(all, " ", &sp) : NULL; t; t = strtok_r(NULL, " ", &sp)) sv_add(&r.jvm, strdup(t));
        free(all);
    }
    size_t cpn = strlen(p.classpath) + 32;
    char* cp = (char*)malloc(cpn);
    if (cp) { snprintf(cp, cpn, "-Djava.class.path=%s", p.classpath); sv_add(&r.jvm, cp); }

    if (jget(args, "game")) {
        add_args(&r.game, jget(args, "game"), vars);
    } else if (jstr(p.json, "minecraftArguments")) {
        char* all = subst(jstr(p.json, "minecraftArguments"), vars);
        char* sp = NULL;
        for (char* t = all ? strtok_r(all, " ", &sp) : NULL; t; t = strtok_r(NULL, " ", &sp)) sv_add(&r.game, strdup(t));
        free(all);
    }

    FILE* lf = fopen(g_log, "w");
    if (lf) {
        fprintf(lf, "Banana OS Minecraft launcher %s - %s as %s (offline), JDK %s\n", LAUNCHER_VER, id, user, jh);
        log_line(lf, "JVM options", &r.jvm);
        fprintf(lf, "main class: %s\n", r.main_class);
        log_line(lf, "game arguments", &r.game);
        fprintf(lf, "----\n");
        fclose(lf);
    }
    plan_free(&p);

    /* the game in its folder, as the official launcher starts it: its log
     * settings write logs/latest.log there (a relative path) */
    if (chdir(mc_root) != 0) { snprintf(err, (size_t)ecap, "cannot go into %s", mc_root); return -1; }

    /* the JVM on a thread with a big stack, as the java app does */
    jvm_started = 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_t t;
    if (pthread_create(&t, &at, run_jvm, &r) != 0) {
        snprintf(err, (size_t)ecap, "cannot start the JVM's thread");
        r.result = -1;
    } else {
        pthread_join(t, NULL);
    }
    sv_free(&r.jvm);
    sv_free(&r.game);
    return r.result;
}
