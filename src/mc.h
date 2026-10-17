/* mc - Minecraft: the version list, downloading a version (client, libraries,
 * assets) the way the official launcher lays them out in ~/.minecraft, and
 * starting it in this process's own JVM (OpenJDK's libjvm.so, dlopen'd).
 *
 * Accounts: offline only for now (any name, the UUID offline servers use). */
#ifndef MCL_MC_H
#define MCL_MC_H

#include <banana.h>

#define MC_JAVA_MAJOR 17                /* what Banana OS's OpenJDK port runs */

typedef struct {
    char id[64];
    char type[16];                      /* release, snapshot, old_beta, old_alpha */
    char url[256];
    char sha1[41];
    char time[32];                      /* releaseTime, ISO 8601 */
    int  java;                          /* Java it needs, if known (0: not known yet) */
    int  lwjgl2;                        /* found to use LWJGL 2 / launchwrapper */
} mc_version;

typedef struct {
    banana_mutex_t lock;
    volatile int   done, total;         /* files */
    volatile int   failed;
    volatile unsigned long bytes;       /* downloaded so far */
    char           status[200];         /* what is happening (read under lock) */
} mc_progress_t;

extern mc_version*   mc_versions;
extern int           mc_nversions;
extern mc_progress_t mc_progress;
extern char          mc_root[256];      /* ~/.minecraft */

/* finds the folders (~/.minecraft); 0, or -1 */
int  mc_init(char* err, int ecap);
/* the version list: Mojang's manifest (saved as a copy, used when offline); 0, or -1 */
int  mc_load_manifest(char* err, int ecap);
/* by its guess from the release date: the Java a version needs (8, 16, 17, 21) */
int  mc_java_guess(const mc_version* v);
/* why Banana OS cannot play a version, as a short tag ("Java 21", "LWJGL 2"), or NULL */
const char* mc_unsupported(const mc_version* v);

/* the JDK: JAVA_HOME, /usr/java/jdk (the java package), /jdk, ... (NULL: none) */
const char* mc_java_home(void);
/* LWJGL for Banana OS: LWJGL_HOME, ~/.minecraft/natives, lwjgl/ beside the JDK (/usr/java/lwjgl), /mnt/<disk>/lwjgl (NULL: none) */
const char* mc_lwjgl_dir(void);
/* things that will stop the game, one per line (empty: none seen) */
void mc_preflight(char* out, int cap);

/* downloads what version `id` needs (skips what is there and whole) */
int  mc_install(const char* id, char* err, int ecap);
/* installs, then runs the game in this process (blocks until it ends):
 * 0, or -1 with err. The game's output goes to ~/.minecraft/logs/banana-launcher.log */
int  mc_play(const char* id, const char* user, int ram_mb, char* err, int ecap);

/* the offline UUID of a player name (32 hex digits) */
void mc_offline_uuid(const char* name, char out[33]);
/* a valid player name: 3-16 of A-Z a-z 0-9 _ */
int  mc_valid_name(const char* name);

void mc_status(const char* fmt, ...);
/* path of the game's log */
const char* mc_log_path(void);

#endif
