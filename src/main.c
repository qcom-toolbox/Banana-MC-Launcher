/* Minecraft launcher for Banana OS: pick a version and a player name, press
 * Play. Downloads the game into ~/.minecraft and runs it in this app's own
 * JVM (OpenJDK 17 from ports/openjdk). Offline accounts only for now. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <banana.h>
#include "mc.h"

#define C_BG      0x15181Du
#define C_HEAD    0x1C2128u
#define C_PANEL   0x20262Fu
#define C_ROW_HI  0x2A323Du
#define C_SEL     0x2F5A3Au
#define C_TEXT    0xE8EEF6u
#define C_DIM     0x8C97A6u
#define C_LINE    0x2C3440u
#define C_FIELD   0x0F1217u
#define C_GREEN   0x3C8D2Fu
#define C_GREENHI 0x4FAA3Eu
#define C_WARN    0xFFD27Au
#define C_ERR     0xFF8A80u
#define C_ACCENT  0x3A7BD5u

#define SANS BANANA_FONT_SANS
#define BOLD BANANA_FONT_SANS_BOLD
#define MONO BANANA_FONT_MONO

#define HEAD_H  60
#define LIST_W  250
#define ROW_H   26
#define PAD     16

enum { ST_LOADING, ST_IDLE, ST_BUSY, ST_RUNNING, ST_DONE, ST_ERROR };

static bwin_t win;
static int fonts;
static volatile int g_state = ST_LOADING;
static char g_msg[300];                 /* the last error or result */
static char g_pre[600];                 /* preflight notes */

/* settings (~/.minecraft/banana-launcher.cfg) */
static char name[17] = "BananaPlayer";
static char sel_id[64] = "1.20.4";
static int  ram = 2048;
static int  snapshots;

/* the list as shown */
static int shown[4096], nshown, sel = -1, scroll;
static int name_focus;
static char logtail[4096];
static unsigned last_log;

/* ── drawing helpers ───────────────────────────────────────────────── */

static int text_w(int font, int size, const char* s) {
    return fonts ? banana_font_width(font, size, s) : (int)strlen(s) * 8;
}

static void text(int x, int y, int font, int size, const char* s, unsigned c) {
    if (fonts) bwin_font(&win, x, y, font, size, s, c);
    else bwin_text(&win, x, y + (size - 8) / 2, s, c, BANANA_TRANSPARENT);
}

/* text cut with "..." to fit w */
static void text_fit(int x, int y, int font, int size, const char* s, int w, unsigned c) {
    if (text_w(font, size, s) <= w) { text(x, y, font, size, s, c); return; }
    char b[300];
    int n = (int)strlen(s);
    if (n > (int)sizeof(b) - 4) n = (int)sizeof(b) - 4;
    while (n > 0) {
        memcpy(b, s, (size_t)n);
        strcpy(b + n, "...");
        if (text_w(font, size, b) <= w) break;
        n--;
    }
    text(x, y, font, size, n ? b : "", c);
}

static void button(int x, int y, int w, int h, const char* label, unsigned bg, unsigned fg, int size) {
    bwin_fill_rect(&win, x, y, w, h, bg);
    bwin_fill_rect(&win, x, y + h - 3, w, 3, bg - 0x101010u);
    int tw = text_w(BOLD, size, label);
    text(x + (w - tw) / 2, y + (h - size) / 2 - 2, BOLD, size, label, fg);
}

static int in(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

/* ── settings ──────────────────────────────────────────────────────── */

static void cfg_path(char* p, int cap) { snprintf(p, (size_t)cap, "%s/banana-launcher.cfg", mc_root); }

static void load_cfg(void) {
    char p[300], line[200];
    cfg_path(p, sizeof(p));
    FILE* f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "name") && mc_valid_name(v)) snprintf(name, sizeof(name), "%s", v);
        else if (!strcmp(line, "version") && v[0]) snprintf(sel_id, sizeof(sel_id), "%s", v);
        else if (!strcmp(line, "ram")) { int r = atoi(v); if (r >= 512 && r <= 16384) ram = r; }
        else if (!strcmp(line, "snapshots")) snapshots = atoi(v) != 0;
    }
    fclose(f);
}

static void save_cfg(void) {
    char p[300];
    cfg_path(p, sizeof(p));
    FILE* f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "name=%s\nversion=%s\nram=%d\nsnapshots=%d\n", name, sel_id, ram, snapshots);
    fclose(f);
}

/* ── the version list ──────────────────────────────────────────────── */

static int list_rows(void) { return (win.h - HEAD_H - PAD - 40 - PAD) / ROW_H; }

static void fix_scroll(void) {
    int rows = list_rows();
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= 0 && sel >= scroll + rows) scroll = sel - rows + 1;
    if (scroll > nshown - rows) scroll = nshown - rows;
    if (scroll < 0) scroll = 0;
}

static void filter_list(void) {
    nshown = 0;
    sel = -1;
    for (int i = 0; i < mc_nversions && nshown < 4096; i++) {
        const char* t = mc_versions[i].type;
        if (strcmp(t, "release") && !(snapshots && !strcmp(t, "snapshot"))) continue;
        if (!strcmp(mc_versions[i].id, sel_id)) sel = nshown;
        shown[nshown++] = i;
    }
    if (sel < 0)        /* the newest one Banana OS can play */
        for (int k = 0; k < nshown && sel < 0; k++)
            if (!mc_unsupported(&mc_versions[shown[k]])) sel = k;
    if (sel >= 0) snprintf(sel_id, sizeof(sel_id), "%s", mc_versions[shown[sel]].id);
    fix_scroll();
}

static void select_row(int k) {
    if (k < 0 || k >= nshown) return;
    sel = k;
    snprintf(sel_id, sizeof(sel_id), "%s", mc_versions[shown[k]].id);
    fix_scroll();
}

/* ── background work ───────────────────────────────────────────────── */

static int bg_manifest(void* a) {
    (void)a;
    char err[200];
    if (mc_load_manifest(err, sizeof(err))) {
        snprintf(g_msg, sizeof(g_msg), "%s", err);
        g_state = ST_ERROR;
        return 0;
    }
    filter_list();
    g_state = ST_IDLE;
    return 0;
}

static int g_play;     /* 1: play, 0: download only */

static int bg_play(void* a) {
    (void)a;
    char err[300] = "";
    int r = g_play ? mc_play(sel_id, name, ram, err, sizeof(err)) : mc_install(sel_id, err, sizeof(err));
    if (r) {
        snprintf(g_msg, sizeof(g_msg), "%s", err);
        g_state = ST_ERROR;
    } else {
        snprintf(g_msg, sizeof(g_msg), g_play ? "Minecraft has ended." : "%s is downloaded.", sel_id);
        g_state = g_play ? ST_DONE : ST_IDLE;
    }
    mc_preflight(g_pre, sizeof(g_pre));
    return 0;
}

static void start(int play) {
    if (g_state == ST_BUSY || g_state == ST_RUNNING || g_state == ST_LOADING || sel < 0) return;
    const char* why = mc_unsupported(&mc_versions[shown[sel]]);
    if (play && why) {
        snprintf(g_msg, sizeof(g_msg), "%s can't run on Banana OS (%s): play 1.7.2 to 1.20.4.", sel_id,
                 !strcmp(why, "LWJGL 2") ? "it uses LWJGL 2: update java.bpk" : !strcmp(why, "Java 8") ? "it needs Java 8" : why);
        g_state = ST_ERROR;
        return;
    }
    if (play && !mc_valid_name(name)) {
        snprintf(g_msg, sizeof(g_msg), "The player name must be 3-16 letters, digits or _.");
        g_state = ST_ERROR;
        return;
    }
    save_cfg();
    g_msg[0] = 0;
    g_play = play;
    g_state = play ? ST_RUNNING : ST_BUSY;
    if (banana_thread(bg_play, NULL) < 0) {
        snprintf(g_msg, sizeof(g_msg), "Cannot start a thread.");
        g_state = ST_ERROR;
    }
}

/* the end of the game's log */
static void read_log(void) {
    FILE* f = fopen(mc_log_path(), "rb");
    logtail[0] = 0;
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    static char raw[sizeof(logtail) * 4];
    long from = n > (long)sizeof(raw) - 1 ? n - (long)sizeof(raw) + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t got = fread(raw, 1, sizeof(raw) - 1, f);
    raw[got] = 0;
    fclose(f);
    /* the game logs log4j XML events (the version's log config, for
     * launchers): keep each message's text, drop the tags */
    size_t o = 0;
    char* sp = NULL;
    for (char* l = strtok_r(raw, "\n", &sp); l; l = strtok_r(NULL, "\n", &sp)) {
        char* s = l;
        while (*s == ' ' || *s == '\t') s++;
        char* c = strstr(s, "<![CDATA[");
        if (c) {
            s = c + 9;
            char* e = strstr(s, "]]>");
            if (e) *e = 0;
            if (!strncmp(s, "[STDERR]: ", 10)) s += 10;
        } else if (*s == '<') {
            continue;
        }
        size_t len = strlen(s);
        if (o + len + 2 > sizeof(logtail)) {          /* keep the newest */
            size_t drop = o + len + 2 - sizeof(logtail);
            if (drop > o) { o = 0; continue; }
            memmove(logtail, logtail + drop, o - drop);
            o -= drop;
        }
        memcpy(logtail + o, s, len);
        o += len;
        logtail[o++] = '\n';
    }
    logtail[o] = 0;
}

/* ── the window ────────────────────────────────────────────────────── */

/* right column geometry */
#define RX (PAD + LIST_W + PAD + 8)
#define RW (win.w - RX - PAD)

static int y_name(void)  { return HEAD_H + PAD + 20; }
static int y_ram(void)   { return y_name() + 32 + 30; }
static int y_play(void)  { return y_ram() + 32 + 22; }
static int y_prog(void)  { return y_play() + 50 + 14; }

static int busy(void) { return g_state == ST_BUSY || g_state == ST_RUNNING || g_state == ST_LOADING; }

static void draw(void) {
    bwin_clear(&win, C_BG);
    char s[300];

    /* header */
    bwin_fill_rect(&win, 0, 0, win.w, HEAD_H, C_HEAD);
    bwin_fill_rect(&win, 0, HEAD_H - 1, win.w, 1, C_LINE);
    bwin_fill_rect(&win, PAD, 16, 28, 28, C_GREEN);                 /* a grass block */
    bwin_fill_rect(&win, PAD, 30, 28, 14, 0x7A5230u);
    bwin_fill_rect(&win, PAD + 6, 26, 6, 6, 0x7A5230u);
    bwin_fill_rect(&win, PAD + 18, 24, 5, 8, 0x7A5230u);
    text(PAD + 40, 12, BOLD, 20, "Minecraft", C_TEXT);
    text(PAD + 40, 36, SANS, 12, "Launcher for Banana OS", C_DIM);
    const char* pill = "OFFLINE MODE";
    int pw = text_w(BOLD, 11, pill) + 20;
    bwin_fill_rect(&win, win.w - PAD - pw, 20, pw, 22, 0x4A3B12u);
    text(win.w - PAD - pw + 10, 23, BOLD, 11, pill, C_WARN);

    /* versions */
    int lx = PAD, ly = HEAD_H + PAD, lh = list_rows() * ROW_H;
    bwin_fill_rect(&win, lx, ly, LIST_W, lh, C_PANEL);
    if (g_state == ST_LOADING && !nshown) text(lx + 12, ly + 12, SANS, 13, "Loading versions...", C_DIM);
    for (int r = 0; r < list_rows() && scroll + r < nshown; r++) {
        int k = scroll + r;
        const mc_version* v = &mc_versions[shown[k]];
        int y = ly + r * ROW_H;
        if (k == sel) bwin_fill_rect(&win, lx, y, LIST_W, ROW_H, C_SEL);
        const char* why = mc_unsupported(v);        /* "Java 21", "LWJGL 2" */
        text_fit(lx + 12, y + 6, k == sel ? BOLD : SANS, 13, v->id, LIST_W - 90, why ? C_DIM : C_TEXT);
        snprintf(s, sizeof(s), "%s", why ? why : strcmp(v->type, "release") ? v->type : "");
        if (s[0]) text(lx + LIST_W - 12 - text_w(SANS, 11, s), y + 7, SANS, 11, s, why ? C_WARN : C_DIM);
    }
    if (nshown > list_rows()) {                                     /* scroll bar */
        int bh = lh * list_rows() / nshown;
        if (bh < 20) bh = 20;
        int by = ly + (lh - bh) * scroll / (nshown - list_rows());
        bwin_fill_rect(&win, lx + LIST_W - 4, by, 3, bh, C_LINE + 0x202020u);
    }
    int cy = ly + lh + 12;
    bwin_fill_rect(&win, lx, cy, 16, 16, snapshots ? C_ACCENT : C_FIELD);
    bwin_rect(&win, lx, cy, 16, 16, C_LINE);
    if (snapshots) text(lx + 4, cy, BOLD, 12, "x", 0xFFFFFFu);
    text(lx + 24, cy, SANS, 13, "Show snapshots", C_TEXT);

    /* player name */
    int y = y_name();
    text(RX, y - 20, SANS, 12, "Player name (offline)", C_DIM);
    bwin_fill_rect(&win, RX, y, RW, 32, C_FIELD);
    bwin_rect(&win, RX, y, RW, 32, name_focus ? C_ACCENT : C_LINE);
    text(RX + 10, y + 8, SANS, 14, name, mc_valid_name(name) ? C_TEXT : C_ERR);
    if (name_focus && (banana_ticks() / 500) % 2 == 0)
        bwin_fill_rect(&win, RX + 11 + text_w(SANS, 14, name), y + 7, 2, 18, C_TEXT);

    /* memory */
    y = y_ram();
    text(RX, y - 20, SANS, 12, "Memory for the game", C_DIM);
    button(RX, y, 32, 32, "-", C_ROW_HI, C_TEXT, 16);
    snprintf(s, sizeof(s), "%d MB", ram);
    text(RX + 44, y + 7, BOLD, 14, s, C_TEXT);
    button(RX + 120, y, 32, 32, "+", C_ROW_HI, C_TEXT, 16);

    /* play / download */
    y = y_play();
    const char* label = g_state == ST_RUNNING ? "Playing..." : g_state == ST_BUSY ? "Downloading..." : "PLAY";
    int dw = 120;
    button(RX, y, RW - dw - 10, 50, label, busy() ? C_ROW_HI : C_GREEN, busy() ? C_DIM : 0xFFFFFFu, 18);
    button(RX + RW - dw, y, dw, 50, "Download", C_ROW_HI, busy() ? C_DIM : C_TEXT, 13);
    if (sel >= 0) {
        snprintf(s, sizeof(s), "%s", mc_versions[shown[sel]].id);
        text_fit(RX + 12, y + 52 - 16, SANS, 11, s, RW - dw - 30, busy() ? C_DIM : 0xCFEFC8u);
    }

    /* progress and messages */
    y = y_prog();
    banana_lock(&mc_progress.lock);
    int done = mc_progress.done, total = mc_progress.total;
    unsigned long bytes = mc_progress.bytes;
    snprintf(s, sizeof(s), "%s", mc_progress.status);
    banana_unlock(&mc_progress.lock);
    if (busy() && total > 0 && done < total) {
        bwin_fill_rect(&win, RX, y, RW, 8, C_FIELD);
        bwin_fill_rect(&win, RX, y, (int)((long)RW * done / total), 8, C_GREENHI);
        char p[300];
        snprintf(p, sizeof(p), "%s  %d / %d  (%lu MB)", s, done, total, bytes >> 20);
        text_fit(RX, y + 14, SANS, 12, p, RW, C_DIM);
    } else if (busy()) {
        text_fit(RX, y + 14, SANS, 12, s, RW, C_DIM);
    }
    y += 40;
    if (g_msg[0]) {
        text_fit(RX, y, BOLD, 13, g_msg, RW, g_state == ST_ERROR ? C_ERR : 0xA8E6A0u);
        y += 24;
    }

    /* notes, then the game's log */
    int box_h = win.h - PAD - y;
    if (box_h < 40) return;
    bwin_fill_rect(&win, RX, y, RW, box_h, C_FIELD);
    int ty = y + 8;
    if (g_state == ST_IDLE || g_state == ST_LOADING || (!logtail[0] && g_pre[0])) {
        if (g_pre[0]) {
            text(RX + 10, ty, BOLD, 12, "Before the game can run:", C_WARN);
            ty += 20;
            char b[600];
            snprintf(b, sizeof(b), "%s", g_pre);
            char* sp = NULL;
            for (char* l = strtok_r(b, "\n", &sp); l && ty < y + box_h - 16; l = strtok_r(NULL, "\n", &sp)) {
                text_fit(RX + 10, ty, SANS, 12, l, RW - 20, C_DIM);
                ty += 18;
            }
        } else {
            text(RX + 10, ty, SANS, 12, "Ready.", C_DIM);
        }
        return;
    }
    /* last lines of the log, bottom up */
    int line_h = 15, max_lines = (box_h - 12) / line_h;
    const char* ends[256];
    int n = 0;
    const char* e = logtail + strlen(logtail);
    while (e > logtail && n < max_lines && n < 256) {
        const char* b = e;
        while (b > logtail && b[-1] != '\n') b--;
        if (b != e) ends[n++] = b;
        e = b > logtail ? b - 1 : b;
    }
    for (int i = n - 1, row = 0; i >= 0; i--, row++) {
        char l[300];
        int len = 0;
        while (ends[i][len] && ends[i][len] != '\n' && ends[i][len] != '\r' && len < 299) { l[len] = ends[i][len]; len++; }
        l[len] = 0;
        for (int k = 0; k < len; k++) if (l[k] == '\t') l[k] = ' ';
        text_fit(RX + 8, y + 6 + row * line_h, MONO, 11, l, RW - 16,
                 strstr(l, "ERROR") || strstr(l, "Exception") ? C_ERR : strstr(l, "WARN") ? C_WARN : 0xB8C2D0u);
    }
}

/* ── input ─────────────────────────────────────────────────────────── */

static void click(int mx, int my) {
    name_focus = in(mx, my, RX, y_name(), RW, 32);
    int ly = HEAD_H + PAD, lh = list_rows() * ROW_H;
    if (in(mx, my, PAD, ly, LIST_W, lh) && !busy()) { select_row(scroll + (my - ly) / ROW_H); return; }
    if (in(mx, my, PAD, ly + lh + 8, LIST_W, 24) && !busy()) { snapshots = !snapshots; filter_list(); return; }
    if (in(mx, my, RX, y_ram(), 32, 32)) { ram = ram > 1024 ? ram - 512 : 512; return; }
    if (in(mx, my, RX + 120, y_ram(), 32, 32)) { ram = ram < 16384 ? (ram < 1024 ? 1024 : ram + 512) : 16384; return; }
    int dw = 120;
    if (in(mx, my, RX, y_play(), RW - dw - 10, 50)) { start(1); return; }
    if (in(mx, my, RX + RW - dw, y_play(), dw, 50)) { start(0); return; }
}

static void key(int k) {
    if (name_focus) {
        int n = (int)strlen(name);
        if (k == '\b') { if (n) name[n - 1] = 0; return; }
        if (k == 27 || k == '\t') { name_focus = 0; return; }
        if (k == '\n' || k == '\r') { name_focus = 0; start(1); return; }
        if (k == 22) {                                              /* Ctrl+V */
            for (const char* s = banana_paste(); *s && n < 16; s++)
                if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_') name[n++] = *s;
            name[n] = 0;
            return;
        }
        if (((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == '_') && n < 16) {
            name[n] = (char)k;
            name[n + 1] = 0;
        }
        return;
    }
    if (busy()) return;
    if (k == BANANA_KEY_UP) select_row(sel - 1);
    else if (k == BANANA_KEY_DOWN) select_row(sel + 1);
    else if (k == BANANA_KEY_PGUP) select_row(sel - list_rows() > 0 ? sel - list_rows() : 0);
    else if (k == BANANA_KEY_PGDN) select_row(sel + list_rows() < nshown ? sel + list_rows() : nshown - 1);
    else if (k == '\n' || k == '\r') start(1);
}

int main(void) {
    char err[200];
    if (bwin_open(&win, "Minecraft Launcher", 760, 500) != 0) {
        printf("The Minecraft launcher is a desktop app: start the desktop first (startx).\n");
        return 1;
    }
    bwin_resizable(&win, 640, 440);
    bwin_wheel(&win);
    fonts = banana_has_fonts();
    if (mc_init(err, sizeof(err))) {
        snprintf(g_msg, sizeof(g_msg), "%s", err);
        g_state = ST_ERROR;
    } else {
        load_cfg();
        mc_preflight(g_pre, sizeof(g_pre));
        if (banana_thread(bg_manifest, NULL) < 0) bg_manifest(NULL);
    }
    for (;;) {
        banana_event_t ev;
        int got = bwin_wait_event(&win, &ev, 250);
        while (got) {
            switch (ev.type) {
            case BANANA_EV_CLOSE:
                save_cfg();
                bwin_close(&win);
                exit(0);                /* also ends a running game */
            case BANANA_EV_MOUSE_DOWN: if (ev.button == 1) click(ev.x, ev.y); break;
            case BANANA_EV_KEY:        key(ev.key); break;
            case BANANA_EV_WHEEL:
                scroll += ev.y * 3;
                if (scroll > nshown - list_rows()) scroll = nshown - list_rows();
                if (scroll < 0) scroll = 0;
                break;
            default: break;
            }
            got = bwin_event(&win, &ev);
        }
        if ((g_state == ST_RUNNING || g_state == ST_DONE || g_state == ST_ERROR) && banana_ticks() - last_log > 500) {
            read_log();
            last_log = banana_ticks();
        }
        draw();
        bwin_update(&win);
    }
}
