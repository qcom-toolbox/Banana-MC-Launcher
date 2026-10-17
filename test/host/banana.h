/* test/host/banana.h - the few Banana OS calls mc.c uses, on Linux (for
 * test/hosttest.c: the launcher's logic tested outside Banana OS) */
#ifndef BANANA_H
#define BANANA_H
typedef volatile int banana_mutex_t;
typedef struct {
    const char* method; const char* url; const char* headers; const char* body;
    unsigned long body_len; const char* content_type; int timeout_ms;
} banana_http_req_t;
typedef struct {
    int status; char* data; unsigned long len; char content_type[96]; char err[128];
} banana_http_resp_t;
int  banana_http_fetch(const banana_http_req_t* req, banana_http_resp_t* resp);
int  banana_http_get(const char* url, char** data, unsigned long* len, char* err, int errcap);
int  banana_thread(int (*fn)(void* arg), void* arg);
int  banana_join(int id);
int  banana_thread_id(void);
void banana_lock(banana_mutex_t* m);
void banana_unlock(banana_mutex_t* m);
#endif
