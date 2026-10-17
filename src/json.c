/* json - a small JSON reader (RFC 8259), enough for Mojang's metadata. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

typedef struct {
    const char* p;
    const char* end;
    const char* start;
    char*       err;
    int         ecap;
    int         depth;
} parser;

static jv* parse_value(parser* ps);

static void fail(parser* ps, const char* what) {
    if (ps->err && ps->err[0] == 0)
        snprintf(ps->err, (size_t)ps->ecap, "JSON: %s at byte %ld", what, (long)(ps->p - ps->start));
}

static void skip_ws(parser* ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static jv* new_value(jtype t) {
    jv* v = (jv*)calloc(1, sizeof(jv));
    if (v) v->t = t;
    return v;
}

static int push(jv* v, char* key, jv* item) {
    if (v->len == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        jv** ni = (jv**)realloc(v->items, (size_t)nc * sizeof(jv*));
        if (!ni) return -1;
        v->items = ni;
        if (v->t == J_OBJ) {
            char** nk = (char**)realloc(v->keys, (size_t)nc * sizeof(char*));
            if (!nk) return -1;
            v->keys = nk;
        }
        v->cap = nc;
    }
    if (v->t == J_OBJ) v->keys[v->len] = key;
    v->items[v->len++] = item;
    return 0;
}

static int hex4(const char* s, unsigned* out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int put_utf8(char* o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63)); o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63));
    o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* a string at ps->p (on the opening quote): malloc'd, unescaped */
static char* parse_string(parser* ps) {
    ps->p++;
    const char* s = ps->p;
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\') ps->p++;
        ps->p++;
    }
    if (ps->p >= ps->end) { fail(ps, "unterminated string"); return NULL; }
    const char* e = ps->p++;
    char* out = (char*)malloc((size_t)(e - s) + 1);     /* escapes only shrink */
    if (!out) { fail(ps, "out of memory"); return NULL; }
    char* o = out;
    while (s < e) {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        char c = *s++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned cp;
            if (e - s < 4 || hex4(s, &cp)) { free(out); fail(ps, "bad \\u escape"); return NULL; }
            s += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                unsigned lo;
                if (!hex4(s + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
            }
            o += put_utf8(o, cp);
            break;
        }
        default: *o++ = c; break;      /* \" \\ \/ */
        }
    }
    *o = 0;
    return out;
}

static jv* parse_container(parser* ps, int obj) {
    if (++ps->depth > 200) { fail(ps, "nested too deep"); return NULL; }
    jv* v = new_value(obj ? J_OBJ : J_ARR);
    if (!v) { fail(ps, "out of memory"); return NULL; }
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) { ps->p++; ps->depth--; return v; }
    for (;;) {
        char* key = NULL;
        skip_ws(ps);
        if (obj) {
            if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected a key"); goto bad; }
            if (!(key = parse_string(ps))) goto bad;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { free(key); fail(ps, "expected ':'"); goto bad; }
            ps->p++;
        }
        jv* item = parse_value(ps);
        if (!item || push(v, key, item)) { free(key); json_free(item); goto bad; }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) { ps->p++; break; }
        fail(ps, obj ? "expected ',' or '}'" : "expected ',' or ']'");
        goto bad;
    }
    ps->depth--;
    return v;
bad:
    json_free(v);
    return NULL;
}

static jv* parse_value(parser* ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end"); return NULL; }
    char c = *ps->p;
    if (c == '{') return parse_container(ps, 1);
    if (c == '[') return parse_container(ps, 0);
    if (c == '"') {
        char* s = parse_string(ps);
        if (!s) return NULL;
        jv* v = new_value(J_STR);
        if (!v) { free(s); return NULL; }
        v->s = s;
        return v;
    }
    if (c == 't' && ps->end - ps->p >= 4 && !strncmp(ps->p, "true", 4)) {
        ps->p += 4;
        jv* v = new_value(J_BOOL);
        if (v) v->n = 1;
        return v;
    }
    if (c == 'f' && ps->end - ps->p >= 5 && !strncmp(ps->p, "false", 5)) { ps->p += 5; return new_value(J_BOOL); }
    if (c == 'n' && ps->end - ps->p >= 4 && !strncmp(ps->p, "null", 4)) { ps->p += 4; return new_value(J_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        int n = 0;
        while (ps->p < ps->end && n < 63 && strchr("+-0123456789.eE", *ps->p)) buf[n++] = *ps->p++;
        buf[n] = 0;
        jv* v = new_value(J_NUM);
        if (v) v->n = strtod(buf, NULL);
        return v;
    }
    fail(ps, "unexpected character");
    return NULL;
}

jv* json_parse(const char* text, unsigned long len, char* err, int ecap) {
    parser ps = { text, text + len, text, err, ecap, 0 };
    if (err && ecap > 0) err[0] = 0;
    /* a UTF-8 byte order mark */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) ps.p += 3;
    jv* v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) { fail(&ps, "trailing data"); json_free(v); return NULL; }
    return v;
}

void json_free(jv* v) {
    if (!v) return;
    for (int i = 0; i < v->len; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->s);
    free(v);
}

jv* jget(const jv* o, const char* key) {
    if (!o || o->t != J_OBJ) return NULL;
    for (int i = 0; i < o->len; i++) if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}

jv* jat(const jv* a, int i) {
    if (!a || (a->t != J_ARR && a->t != J_OBJ) || i < 0 || i >= a->len) return NULL;
    return a->items[i];
}

const char* jstr(const jv* o, const char* key) {
    jv* v = jget(o, key);
    return v && v->t == J_STR ? v->s : NULL;
}

double jnum(const jv* o, const char* key, double def) {
    jv* v = jget(o, key);
    return v && (v->t == J_NUM || v->t == J_BOOL) ? v->n : def;
}

int jbool(const jv* o, const char* key, int def) {
    jv* v = jget(o, key);
    return v && v->t == J_BOOL ? (int)v->n : def;
}

int jlen(const jv* v) {
    return v && (v->t == J_ARR || v->t == J_OBJ) ? v->len : 0;
}
