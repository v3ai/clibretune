/* lt_codes.h - shared integer result codes for all clibretune-C modules.
 *
 * Convention (docs/CONVENTIONS.md): functions that can fail return `int`,
 * 0 = success, negative = one of the codes below. A human-readable message is
 * stored with lt_err_set() and read back with lt_err_get(). Functions that
 * return pointers return NULL on failure (message set the same way).
 * Modules may add more specific detail (e.g. lt_proto_errinfo) but must still
 * return one of these generic codes.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_CODES_H
#define LT_CODES_H

#define LT_OK            0
#define LT_EINVAL       (-1)   /* bad argument / bad user input */
#define LT_ENOENT       (-2)   /* file, name or page not found */
#define LT_EEXIST       (-3)   /* already exists */
#define LT_EIO          (-4)   /* file / stream I/O failure */
#define LT_EDATA        (-5)   /* malformed data (parse error, bad checksum) */
#define LT_EBUSY        (-6)   /* operation refused in the current state */
#define LT_ETIMEOUT     (-7)   /* no (complete) reply within the deadline */
#define LT_ECANCEL      (-8)   /* cancelled via a cancel flag / progress cb */
#define LT_EPROTO       (-9)   /* ECU replied, but not as the protocol requires */
#define LT_ENOTCONN     (-10)  /* no open connection / stream closed */
#define LT_ERANGE       (-11)  /* index/offset/value outside allowed range */
#define LT_EUNSUPPORTED (-12)  /* feature not implemented for this input */
#define LT_ECRC         (-13)  /* CRC / checksum mismatch (also see EDATA) */

#endif /* LT_CODES_H */
