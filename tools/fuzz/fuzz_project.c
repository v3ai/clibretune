/* fuzz_project.c - project files: project.properties (Java properties, UTF-8 or Latin-1),
 * project.json, action scripts, INI header sniffing and restore point names. */
#include "fuzz_common.h"
#include "project/project.h"
#include "project/proj_priv.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char err[256], ts[21], path[512];
    char *s = fz_cstr(data, size), *z, *sig = NULL, *nm = NULL;
    lt_props pr;
    lt_proj_config cfg;
    lt_action_set as;
    int iv;
    double dv;
    lt_bool bv;

    lt_props_init(&pr);
    lt_props_parse(&pr, s);
    (void)lt_props_get_i32(&pr, "baudRate", &iv);
    (void)lt_props_get_f64(&pr, "x", &dv);
    (void)lt_props_get_bool(&pr, "y", &bv);
    lt_props_set(&pr, "k", "v=1\\ \n");
    z = lt_props_to_string(&pr);
    lt_free(z);
    lt_props_free(&pr);

    snprintf(path, sizeof path, "%s/p.properties", fz_tmpdir());
    fz_write(path, data, size);
    lt_props_init(&pr);
    (void)lt_props_load(&pr, path);
    lt_props_free(&pr);

    snprintf(path, sizeof path, "%s/project.json", fz_tmpdir());
    fz_write(path, data, size);
    memset(&cfg, 0, sizeof cfg);
    if (lt_proj_config_read(&cfg, path) == 0) lt_proj_config_write(&cfg, path);
    lt_proj_config_clear(&cfg);

    lt_action_set_init(&as);
    if (lt_action_set_deserialize(s, &as, err, sizeof err) == 0) {
        z = lt_action_set_serialize(&as, err, sizeof err);
        lt_free(z);
    }
    lt_action_set_free(&as);

    if (lt_ini_extract_info(s, &sig, &nm) == 0) { lt_free(sig); lt_free(nm); }
    (void)lt_restore_parse_timestamp(s, ts);
    free(s);
    return 0;
}
