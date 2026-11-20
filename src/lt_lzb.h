/* lt_lzb.h - decoder of the byte-oriented LZ77 format that tools/lzpack.py writes; used for the
 * data embedded in the program (base tunes: src/tune/basetune_data.c, ECU definitions:
 * src/project/inidb_data.c).
 *
 * Format: a sequence of
 *   token     high nibble literal count, low nibble match length - 4; 15 = more length bytes
 *             follow (each 255 adds 255 and continues, the first other value ends the count)
 *   literals
 *   distance  3 bytes little endian back into the output - absent once the output is complete
 *   match     (length continuation bytes when the low nibble is 15)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_LZB_H
#define LT_LZB_H

#include "lt.h"

LT_BEGIN_DECLS

/* Unpack exactly outlen bytes into out. 0 ok, -1 on malformed or truncated input (never reads
 * or writes out of bounds). */
int lt_lzb_decode(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen);
/* The same with a preset history: buf holds `hist` bytes that the packer saw before the data
 * (another file); outlen bytes are written at buf + hist and back references may reach into
 * the history. buf must have room for hist + outlen bytes. 0 ok, -1 malformed. */
int lt_lzb_decode_hist(const unsigned char *in, size_t inlen, unsigned char *buf, size_t hist,
                       size_t outlen);

LT_END_DECLS

#endif
