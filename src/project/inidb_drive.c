/* inidb_drive.c - the INI a rusEFI board carries on its own USB mass-storage drive.
 *
 * rusEFI firmware (firmware/hw_layer/mass_storage/, bin/gen_image_board.sh) can present a small
 * read-only FAT volume labelled RUSEFI (SCSI inquiry "rusEFI" / "INI Drive"):
 *   - EFI_USE_COMPRESSED_INI_MSD (the F7/H7 default where the drive is enabled): the volume holds
 *     the plain generated INI (rusefi_<board>.ini) plus README and .url files - readable here;
 *   - the uncompressed image: the INI only as rusefi.ini.7z (7-Zip, LZMA), which LibreTune has no
 *     decoder for (older firmware used rusefi.ini.zip, deflate - no decoder either);
 *   - since 2026 the F4 boards build without the drive (EFI_EMBED_INI_MSD FALSE).
 * The OS mounts the volume; lt_plat_volume_list (per-OS file) lists the mounted volumes and this
 * universal code looks at their top level only.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "inidb.h"
#include "inilib.h"
#include "proj_priv.h"
#include "../plat/plat.h"

#define MAX_INI_BYTES 8000000L

static lt_bool same_sig(const char *a, const char *b)
{
    char x[256], y[256];
    if (lt_sig_normalize(a, x, sizeof(x)) == 0) { return LT_FALSE; }
    (void)lt_sig_normalize(b, y, sizeof(y));
    return strcmp(x, y) == 0;
}

/* the signature of the INI file at path matches; 1 yes, 0 no */
static int ini_matches(const char *path, const char *signature)
{
    long sz = lt_proj_file_size(path);
    char *raw, *txt, *sig = NULL;
    size_t n = 0, tl = 0;
    int ok = 0;
    if (sz <= 0 || sz > MAX_INI_BYTES) { return 0; }
    raw = lt_file_read(path, &n);
    if (raw == NULL) { return 0; }
    txt = lt_ini_decode((const lt_u8 *)raw, n, &tl);
    lt_free(raw);
    if (txt == NULL) { return 0; }
    if (lt_ini_extract_info(txt, &sig, NULL) == LT_OK) {
        ok = same_sig(sig, signature);
        lt_free(sig);
    }
    lt_free(txt);
    return ok;
}

/* 1 found (path filled), 0 not; *compressed set when only a packed INI was seen */
static int scan_volume(const char *vol, const char *signature, char *path, size_t cap,
                       int *compressed)
{
    char **names = NULL;
    int n = lt_plat_list_dir(vol, &names), i, found = 0;
    for (i = 0; i < n && !found; i++) {
        const char *nm = names[i];
        if (lt_stricmp(nm, "rusefi.ini.7z") == 0 || lt_stricmp(nm, "rusefi.ini.zip") == 0) {
            *compressed = 1;
        } else if (lt_stricmp(lt_path_ext(nm), ".ini") == 0) {
            char *p = lt_path_join(vol, nm);
            if (lt_proj_is_file(p) && ini_matches(p, signature)) {
                lt_strlcpy(path, p, cap);
                found = 1;
            }
            lt_free(p);
        }
    }
    if (n > 0) { lt_plat_free_list(names, n); }
    return found;
}

int lt_inidb_drive_find(const char *signature, char *path, size_t cap, int *compressed)
{
    char *env = lt_plat_getenv("LIBRETUNE_VOLUMES");
    int comp = 0, found = 0;
    if (cap > 0) { path[0] = '\0'; }
    if (signature != NULL && signature[0] != '\0') {
        if (env != NULL) {
            char *s = env;
            while (!found && *s != '\0') {
                char *e = strchr(s, ';');
                if (e != NULL) { *e = '\0'; }
                if (*s != '\0') { found = scan_volume(s, signature, path, cap, &comp); }
                if (e == NULL) { break; }
                s = e + 1;
            }
        } else {
            char **vols = NULL;
            int nv = lt_plat_volume_list(&vols), i;
            for (i = 0; i < nv && !found; i++) {
                found = scan_volume(vols[i], signature, path, cap, &comp);
            }
            if (nv > 0) { lt_plat_free_list(vols, nv); }
        }
    }
    lt_free(env);
    if (compressed != NULL) { *compressed = found ? 0 : comp; }
    return found;
}
