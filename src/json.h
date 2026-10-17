/* json - a small JSON reader: the whole document into a tree. */
#ifndef MCL_JSON_H
#define MCL_JSON_H

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jv {
    jtype        t;
    double       n;          /* J_NUM, J_BOOL (0 / 1) */
    char*        s;          /* J_STR */
    int          len;        /* J_ARR, J_OBJ: how many items */
    int          cap;
    struct jv**  items;      /* J_ARR, J_OBJ */
    char**       keys;       /* J_OBJ */
} jv;

/* the document, or NULL (err: what and where) */
jv*         json_parse(const char* text, unsigned long len, char* err, int ecap);
void        json_free(jv* v);

jv*         jget(const jv* obj, const char* key);          /* NULL if missing / not an object */
jv*         jat(const jv* arr, int i);                     /* NULL if out of range */
const char* jstr(const jv* obj, const char* key);          /* NULL unless a string */
double      jnum(const jv* obj, const char* key, double def);
int         jbool(const jv* obj, const char* key, int def);
int         jlen(const jv* v);                             /* items of an array / object, else 0 */

#endif
