/* lt_lzb.c - LZ77 decoder for the embedded data (format: lt_lzb.h, encoder: tools/lzpack.py).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_lzb.h"

/* length continuation bytes after a nibble of 15; 0 ok, -1 truncated or absurd */
static int lzb_more(const unsigned char *in, size_t inlen, size_t *ip, size_t *v)
{
    unsigned char c;
    do {
        if (*ip >= inlen) { return -1; }
        c = in[(*ip)++];
        if (!lt_add_size(*v, (size_t)c, v)) { return -1; }
    } while (c == 255);
    return 0;
}

int lt_lzb_decode_hist(const unsigned char *in, size_t inlen, unsigned char *buf, size_t hist,
                       size_t outlen)
{
    size_t ip = 0, op = hist, end;
    if ((in == NULL && inlen > 0) || (buf == NULL && (hist > 0 || outlen > 0))) { return -1; }
    if (!lt_add_size(hist, outlen, &end)) { return -1; }
    for (;;) {
        unsigned t;
        size_t lit, dist, mlen, k;
        if (ip >= inlen) { return -1; }
        t = in[ip++];
        lit = (size_t)(t >> 4);
        if (lit == 15 && lzb_more(in, inlen, &ip, &lit) != 0) { return -1; }
        if (lit > inlen - ip || lit > end - op) { return -1; }
        if (lit > 0) { memcpy(buf + op, in + ip, lit); }
        ip += lit;
        op += lit;
        if (op == end) { return 0; }
        if (inlen - ip < 3) { return -1; }
        dist = (size_t)in[ip] | ((size_t)in[ip + 1] << 8) | ((size_t)in[ip + 2] << 16);
        ip += 3;
        mlen = (size_t)(t & 15U) + 4;
        if ((t & 15U) == 15U && lzb_more(in, inlen, &ip, &mlen) != 0) { return -1; }
        if (dist == 0 || dist > op || mlen > end - op) { return -1; }
        for (k = 0; k < mlen; k++) { buf[op + k] = buf[op - dist + k]; }   /* may overlap */
        op += mlen;
    }
}

int lt_lzb_decode(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen)
{
    return lt_lzb_decode_hist(in, inlen, out, 0, outlen);
}
