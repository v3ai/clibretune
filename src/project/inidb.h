/* inidb.h - the ECU definitions (INI files) bundled with the program, and picking the right
 * definition for an ECU signature. UI-free.
 *
 *   catalog      lt_inidb_count / lt_inidb_at / lt_inidb_filter: family, board, version,
 *                signature and size of every bundled file (src/project/inidb_data.c, generated
 *                by tools/gen_definitions.py; licences and sources in assets/definitions/)
 *   lookup       lt_inidb_find: bundled definitions with exactly this signature
 *   files        lt_inidb_text unpacks one; lt_inidb_materialise puts it into the local INI
 *                repository (lt_ini_repo), so code that wants a path keeps working
 *   signatures   lt_sig_parse: family / board / build date of Speeduino and rusEFI-style
 *                signatures; lt_inidb_distance: how far apart two builds are
 *   resolve      lt_inidb_resolve: the best definition for an ECU signature - exact in the
 *                ECU's own USB drive, the user's repository or the bundle; else the nearest
 *                build of the same family and board ("near", the UI warns); else none
 *   USB drive    lt_inidb_drive_find: rusEFI boards with an embedded INI drive (F7/H7 builds
 *                with EFI_USE_COMPRESSED_INI_MSD) show a volume "RUSEFI" holding the plain
 *                rusefi_<board>.ini; boards with the uncompressed image only hold
 *                rusefi.ini.7z (7-Zip/LZMA), which LibreTune cannot read and reports as such
 *
 * Typical use (connect flow):
 *     lt_inidb_pick pk; char *path = NULL;
 *     if (lt_inidb_resolve(ecu_sig, &repo, LT_INIDB_USE_DRIVE, &pk) != LT_INIDB_NONE
 *         && lt_inidb_pick_path(&repo, &pk, &path) == 0) { ... use path ...; lt_free(path); }
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_INIDB_H
#define LT_INIDB_H

#include "project.h"

LT_BEGIN_DECLS

typedef struct lt_inidb_def {
    const char   *family;     /* "speeduino", "rusefi", "fome", "miata-ecus" */
    const char   *board;      /* "proteus_f4"; "" for Speeduino */
    const char   *version;    /* tag(s): "202501.6", "release_20260812", "202501 to 202501.4" */
    const char   *signature;  /* the INI's [MegaTune] signature */
    const char   *file;       /* upstream file name ("rusefi_proteus_f4.ini") */
    const char   *source;     /* where it comes from (release page, tag URL) */
    const char   *licence;    /* SPDX id of the file's licence */
    unsigned long size;       /* bytes */
    lt_u32        crc32;      /* lt_crc32 of the file */
    int           hist0, hist1;   /* definitions the file is packed against (-1 none) */
    unsigned long zoff, zlen; /* packed bytes in the stream lt_inidb_z */
} lt_inidb_def;

/* generated data (inidb_data.c) */
extern const lt_inidb_def lt_inidb_tab[];
extern const size_t lt_inidb_tab_n;
extern const unsigned char *const lt_inidb_z[];   /* the packed stream in blocks <= 32767 bytes */
extern const size_t lt_inidb_zlen[];
extern const size_t lt_inidb_zn;
extern const size_t lt_inidb_ztotal;

size_t lt_inidb_count(void);
const lt_inidb_def *lt_inidb_at(size_t i);                 /* NULL when out of range */
long  lt_inidb_index(const lt_inidb_def *d);               /* position in the catalog or -1 */
/* "Speeduino", "rusEFI", "FOME", "miata-ecus" (the family id itself when unknown) */
const char *lt_inidb_family_title(const char *family);
/* one line for lists: "rusEFI proteus_f4 - release_20260812" / "Speeduino 202501.6" */
void  lt_inidb_label(const lt_inidb_def *d, char *out, size_t cap);
/* query: case-insensitive words that must all occur in family title, board, version, signature */
lt_bool lt_inidb_match(const lt_inidb_def *d, const char *query);
/* catalog indexes that match, in catalog order; writes up to cap, returns the total */
size_t lt_inidb_filter(const char *query, size_t *out, size_t cap);
/* bundled definitions whose signature equals sig (case and punctuation insensitive, like
 * lt_sig_normalize), newest first; writes up to cap indexes, returns the total */
size_t lt_inidb_find(const char *sig, size_t *out, size_t cap);
/* the file as a malloc'd NUL-terminated buffer, byte for byte as upstream ships it (CRC checked);
 * *len (may be NULL) = size. NULL + lt_err when the embedded data is damaged. */
char *lt_inidb_text(const lt_inidb_def *d, size_t *len);
/* Put d into the local INI repository (no-op when its signature is already there); *id_out
 * (may be NULL) gets the repository id (malloc'd). 0 or negative + lt_err. */
int   lt_inidb_materialise(lt_ini_repo *r, const lt_inidb_def *d, char **id_out);

/* ---- signatures ---- */
#define LT_SIGFAM_UNKNOWN   0
#define LT_SIGFAM_SPEEDUINO 1   /* "speeduino 202501" */
#define LT_SIGFAM_RUSEFI    2   /* "rusEFI master.2026.08.13.proteus_f4.357168325",
                                   "rusEFI (FOME) .2026.06.03.atlas.3685778166" */
typedef struct lt_sig_info {
    int  family;                /* LT_SIGFAM_* */
    char variant[24];           /* rusEFI-style: the "(FOME)" / "(epicEFI)" part, "" = rusEFI */
    char branch[48];            /* rusEFI-style: "master", "main", "" */
    char board[64];             /* rusEFI-style: "proteus_f4" */
    long when;                  /* build date as a comparable number: rusEFI days
                                   (y*372 + m*31 + d), Speeduino months (y*12 + m - 1) */
} lt_sig_info;
/* 0 when the signature is recognised (out->family != LT_SIGFAM_UNKNOWN), else -1 */
int  lt_sig_parse(const char *sig, lt_sig_info *out);
/* 0 when the signatures are equal (lt_sig_normalize), the build distance (rusEFI days,
 * Speeduino months; at least 1) when both are the same family, variant and board, else -1 */
long lt_inidb_distance(const char *ecu_sig, const char *ini_sig);

/* ---- resolve ---- */
#define LT_INIDB_NONE  0
#define LT_INIDB_NEAR  1
#define LT_INIDB_EXACT 2

#define LT_INIDB_SRC_NONE   0
#define LT_INIDB_SRC_DRIVE  1       /* the ECU's own USB drive */
#define LT_INIDB_SRC_REPO   2       /* the user's INI repository */
#define LT_INIDB_SRC_BUNDLE 3       /* bundled with the program */

#define LT_INIDB_USE_DRIVE  1       /* flags: look for the ECU's USB drive first */

typedef struct lt_inidb_pick {
    int  quality;               /* LT_INIDB_EXACT / NEAR / NONE */
    int  src;                   /* LT_INIDB_SRC_* */
    long bundle;                /* catalog index (src BUNDLE), else -1 */
    long distance;              /* lt_inidb_distance (0 exact) */
    int  drive_compressed;      /* a rusEFI drive was seen that only has a compressed INI */
    char repo_id[160];          /* src REPO */
    char path[512];             /* src DRIVE: the INI file on the drive */
    char signature[160];        /* the picked definition's signature */
    char label[200];            /* "rusEFI proteus_f4 - release_20260812 (bundled)" */
} lt_inidb_pick;

/* The best definition for an ECU reporting `signature`, in this order:
 *   1. (flags & LT_INIDB_USE_DRIVE) an INI with exactly this signature on a mounted USB drive
 *   2. exact in the repository (repo may be NULL), 3. exact in the bundle (newest first)
 *   4. near: the closest build of the same family/variant/board in the repository or the
 *      bundle (rusEFI: closest build date of that board; Speeduino: closest release; ties go
 *      to the newer build, then to the repository)
 * Fills *out (always) and returns its quality. */
int  lt_inidb_resolve(const char *signature, const lt_ini_repo *repo, int flags,
                      lt_inidb_pick *out);
/* The picked definition as a file in the repository: bundled ones are materialised, a drive's
 * INI is imported (kept for offline use). *path_out malloc'd. 0 or negative + lt_err. */
int  lt_inidb_pick_path(lt_ini_repo *repo, const lt_inidb_pick *p, char **path_out);

/* ---- the ECU's USB drive (inidb_drive.c) ---- */
/* Mounted volumes to look at: $LIBRETUNE_VOLUMES (';'-separated directories) when set, else
 * lt_plat_volume_list. Looks at the top level of each for *.ini files whose signature equals
 * `signature` (lt_sig_normalize). 1 found (path filled), 0 not found. *compressed (may be NULL)
 * is set when a volume holds rusefi.ini.7z / rusefi.ini.zip but no plain INI. */
int  lt_inidb_drive_find(const char *signature, char *path, size_t cap, int *compressed);

LT_END_DECLS

#endif
