/* fuzz_tsimport.c - TunerStudio project import. The input is split at "\n#~#~\n" into
 * projectCfg/project.properties, projectCfg/mainController.ini, CurrentTune.msq and
 * projectCfg/pcVariableValues.msq of a scratch TS project; then the import preview and the
 * import itself run against it (a new LibreTune project is created and removed again). */
#include "fuzz_common.h"
#include "project/project.h"
#include "app/files_logic.h"

static const char SEP[] = "\n#~#~\n";

static const char *find_sep(const char *s, const char *end)
{
    size_t k = sizeof SEP - 1;
    for (; s + k <= end; s++)
        if (memcmp(s, SEP, k) == 0) return s;
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char *files[4] = { "projectCfg/project.properties",
        "projectCfg/mainController.ini", "CurrentTune.msq", "projectCfg/pcVariableValues.msq" };
    char ts[512], out[512], path[700], cmd[1200];
    const char *s = (const char *)data, *end = s + size;
    int i;
    lt_ts_preview pv;
    lt_project p;
    snprintf(ts, sizeof ts, "%s/ts", fz_tmpdir());
    snprintf(out, sizeof out, "%s/out", fz_tmpdir());
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s' && mkdir -p '%s/projectCfg' '%s'", ts, out, ts, out);
    if (system(cmd) != 0) abort();
    for (i = 0; i < 4 && s != NULL; i++) {
        const char *e = find_sep(s, end);
        snprintf(path, sizeof path, "%s/%s", ts, files[i]);
        fz_write(path, s, (size_t)((e ? e : end) - s));
        s = e ? e + sizeof SEP - 1 : NULL;
    }
    (void)lt_ts_preview_read(ts, &pv);
    memset(&p, 0, sizeof p);
    if (lt_project_import_ts(ts, out, &p) == 0) lt_project_free(&p);
    return 0;
}
