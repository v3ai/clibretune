/* files_logic.h - tune and project file helpers without UI: the TunerStudio import preview (G-18)
 * and the tune file list / tune info of the open-tune dialogs (G-19).  Unit-tested in
 * tests/test_files.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_FILES_LOGIC_H
#define LT_FILES_LOGIC_H

#include "../lt.h"

/* ---- TunerStudio project preview ---- */
typedef struct lt_ts_preview {
    char name[96];
    char ini_file[128];          /* ecuConfigFile of project.properties ("" = none specified) */
    int  ini_found;              /* that file exists in projectCfg/ */
    char signature[96];          /* signature of that INI ("" unknown) */
    int  has_tune;               /* CurrentTune.msq */
    int  has_pcv;                /* projectCfg/pcVariableValues.msq */
    int  n_restore;              /* restorePoints, one per .msq */
    int  n_dash;                 /* .dash dashboards found */
    char port[64];
    int  baud;
} lt_ts_preview;
/* 0 ok, LT_ENOENT "Not a valid TS project" when project.properties is missing */
int lt_ts_preview_read(const char *ts_dir, lt_ts_preview *out);
/* copy the dash and gauge files of a TS project into <project>/dashboards/; returns the count */
int lt_ts_import_dashboards(const char *ts_dir, const char *project_dir);

/* ---- tune files ---- */
typedef struct lt_tuneinfo {
    char file[128];
    char signature[96], version[16], saved[48], author[64], desc[160];
    unsigned long size;
    size_t constants, pages;
} lt_tuneinfo;
/* header facts of an .msq file; 0 ok or negative with lt_err */
int lt_tuneinfo_read(const char *path, lt_tuneinfo *out);
/* *.msq / *.xml files directly inside dir, sorted by name; malloc'd names (lt_strv_free) */
int lt_tunes_list(const char *dir, char ***names, size_t *n);
/* "name.msq" of a free file name in dir for a new tune called `name` ("name.msq",
 * "name 2.msq", ...); characters that are not valid in file names become '_' */
void lt_tunes_new_name(const char *dir, const char *name, char *out, size_t cap);

#endif
