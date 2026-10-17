/* hash - MD5 (offline UUIDs) and SHA-1 (checking downloads). */
#ifndef MCL_HASH_H
#define MCL_HASH_H

void md5(const void* data, unsigned long len, unsigned char out[16]);
void sha1(const void* data, unsigned long len, unsigned char out[20]);
void hex(const unsigned char* b, int n, char* out);      /* out: 2n + 1 bytes */

#endif
