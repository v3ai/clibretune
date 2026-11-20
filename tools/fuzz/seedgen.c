/* seedgen.c - writes synthetic seed inputs for the fuzz harnesses that have no file fixtures
 * (binary logs, firmware images, protocol replies, ...) using the library's own writers.
 *   seedgen OUTDIR    -> OUTDIR/<harness>/<seed files>   (run by tools/fuzz/run.sh) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "datalog/datalog.h"
#include "dash/dash.h"
#include "fw/fw.h"
#include "proto/proto.h"
#include "tune/tune.h"
#include "tune/tune_csv.h"

static const char *g_out;

static void put(const char *harness, const char *name, const void *d, size_t n, int prefix)
{
    char path[600], cmd[700];
    FILE *f;
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/%s'", g_out, harness);
    if (system(cmd) != 0) exit(1);
    snprintf(path, sizeof path, "%s/%s/%s", g_out, harness, name);
    f = fopen(path, "wb");
    if (!f) exit(1);
    if (prefix >= 0) fputc(prefix, f);
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

static void put_file(const char *harness, const char *name, const char *src, int prefix)
{
    size_t n = 0;
    char *d = lt_file_read(src, &n);
    if (!d) return;
    put(harness, name, d, n, prefix);
    lt_free(d);
}

static void logs(void)
{
    static const char *nm[4] = { "RPM", "MAP", "AFR", "CLT" };
    static const char *un[4] = { "rpm", "kPa", "AFR", "C" };
    lt_log l;
    lt_buf b;
    double v[4];
    int i;
    char path[600];
    lt_log_init(&l);
    lt_log_set_channels(&l, nm, un, 4);
    for (i = 0; i < 30; i++) {
        v[0] = 800 + 50 * i; v[1] = 30 + i; v[2] = 14.7 - i * 0.05; v[3] = 80;
        lt_log_append(&l, i * 0.1, v);
    }
    lt_log_add_marker(&l, 1.0, "mark");
    lt_log_set_header(&l, "seed log");
    snprintf(path, sizeof path, "%s/seed.mlg", g_out);
    if (lt_mlg_write(path, &l, "info") == 0) put_file("mlg", "seed.mlg", path, -1);
    lt_buf_init(&b);
    lt_msl_format(&b, &l);
    put("msl", "seed.msl", b.data, b.len, -1);
    lt_buf_free(&b);
    snprintf(path, sizeof path, "%s/seed.csv", g_out);
    if (lt_csv_write(path, &l) == 0) put_file("csv", "seed.csv", path, -1);
    lt_log_free(&l);
    put_file("csv", "ts_style.csv", "tests/fixtures/datalog/ts_style.csv", -1);
    put_file("csv", "dyno.csv", "tests/fixtures/dyno/run_after.csv", -1);
    put_file("msl", "ts_style.csv", "tests/fixtures/datalog/ts_style.csv", -1);
}

static void dashes(void)
{
    lt_dash_file *d[3];
    lt_buf b;
    int i;
    char name[32];
    d[0] = lt_dash_template_basic();
    d[1] = lt_dash_template_tuning();
    d[2] = lt_dash_template_telemetry();
    for (i = 0; i < 3; i++) {
        lt_buf_init(&b);
        lt_dash_write(d[i], &b);
        snprintf(name, sizeof name, "tmpl%d.dash", i);
        put("dash", name, b.data, b.len, -1);
        lt_buf_free(&b);
        if (i == 0 && d[i]->cluster.n_comps > 0 && d[i]->cluster.comps[0].gauge) {
            lt_gauge_file g;
            memset(&g, 0, sizeof g);
            g.gauge = *d[i]->cluster.comps[0].gauge;
            lt_buf_init(&b);
            lt_gaugefile_write(&g, &b);
            put("dash", "one.gauge", b.data, b.len, -1);
            lt_buf_free(&b);
        }
        lt_dash_free(d[i]);
    }
    put_file("dash", "basic.dash", "tests/fixtures/dash/basic.dash", -1);
}

static void firmware(void)
{
    lt_fw_image img;
    lt_u8 data[600];
    lt_buf b;
    char err[100];
    size_t i;
    for (i = 0; i < sizeof data; i++) data[i] = (lt_u8)(i * 7u);
    data[0] = 0x00; data[1] = 0x00; data[2] = 0x02; data[3] = 0x20;   /* SP 0x20020000 */
    data[4] = 0xC1; data[5] = 0x01; data[6] = 0x00; data[7] = 0x08;   /* reset 0x080001C1 */
    lt_fw_image_init(&img);
    lt_fw_image_add(&img, 0x08000000UL, data, 300, err, sizeof err);
    lt_fw_image_add(&img, 0x08008000UL, data, 200, err, sizeof err);
    lt_fw_image_add(&img, 0x20000000UL, data + 100, 37, err, sizeof err);
    lt_fw_image_finish(&img, err, sizeof err);
    img.entry = 0x080001C1UL; img.has_entry = 1;
    lt_buf_init(&b);
    lt_fw_write_ihex(&img, &b);
    put("fw", "seed.hex", b.data, b.len, LT_FW_FMT_IHEX);
    put("fw", "seed_detect.hex", b.data, b.len, LT_FW_FMT_UNKNOWN);
    lt_buf_clear(&b);
    lt_fw_write_srec(&img, &b);
    put("fw", "seed.srec", b.data, b.len, LT_FW_FMT_SREC);
    lt_buf_clear(&b);
    lt_fw_write_dfuse(&img, 0x0483, 0xDF11, 0, "Internal Flash", &b);
    put("fw", "seed.dfu", b.data, b.len, LT_FW_FMT_DFUSE);
    put("fw", "seed_detect.dfu", b.data, b.len, LT_FW_FMT_UNKNOWN);
    lt_buf_free(&b);
    put("fw", "seed.bin", data, sizeof data, LT_FW_FMT_BIN);
    lt_fw_image_free(&img);
}

static void proto(void)
{
    lt_buf b, f;
    lt_u8 pl[200];
    size_t i;
    int order;
    for (order = 0; order < 2; order++) {
        lt_buf_init(&b);
        pl[0] = 0;
        memcpy(pl + 1, "speeduino 202501", 16);
        lt_buf_init(&f);
        lt_env_encode((lt_env_order)order, pl, 17, &f);       /* handshake */
        lt_buf_append(&b, f.data, f.len);
        for (i = 0; i < 12; i++) {                            /* page chunks, acks, realtime */
            size_t n = i % 3 == 0 ? 33 : (i % 3 == 1 ? 1 : 21), k;
            pl[0] = 0;
            for (k = 1; k < n; k++) pl[k] = (lt_u8)(k * 3 + i);
            lt_buf_clear(&f);
            lt_env_encode((lt_env_order)order, pl, n, &f);
            lt_buf_append(&b, f.data, f.len);
        }
        lt_buf_free(&f);
        put("proto", order ? "le.bin" : "be.bin", b.data, b.len, order);
        lt_buf_free(&b);
    }
    put("proto", "legacy.bin", "speeduino 202501\x00\x01\x02", 19, 0x80);
}

static void tunes(void)
{
    static const char csv[] =
        "Name,Page,Offset,Shape,Value,Units,Min,Max,Scale,Translate,DataType,IsPcVariable\n"
        "reqFuel,1,0,scalar,10.5,ms,0,25.5,0.1,0,U08,false\n"
        "veTable,1,4,16x16,\"[1,2,3,4]\",%,0,255,1,0,U08,false\n"
        "nCylinders,1,2,bits,\"4\",,0,0,1,0,Bits,false\n";
    put("msq", "a.csv", csv, sizeof csv - 1, 3);
    put_file("msq", "sample.msq", "tests/fixtures/tune/sample.msq", 0);
    put_file("msq", "ts.msq", "tests/fixtures/project/ts_proj/CurrentTune.msq", 0);
    put_file("msq", "pcv.msq", "tests/fixtures/project/ts_proj/projectCfg/pcVariableValues.msq", 0);
    put_file("msq", "real.table", "tests/fixtures/tune/real_export.table", 1);
    put_file("msq", "sample.tuneView", "tests/fixtures/tune/sample.tuneView", 2);
    {
        lt_ini *ini = lt_ini_load_file("tests/fixtures/tune/mini_speeduino.ini", NULL);
        lt_tune *t = ini ? lt_tune_from_defaults(ini) : NULL;
        if (t) {
            static const char *const names[4] = { "veTable", "reqFuel", "nCylinders", "unknownX" };
            /* (names the INI lacks are skipped) */
            lt_tune *part = lt_tune_part_extract(t, names, 4);
            lt_buf b;
            lt_buf_init(&b);
            lt_tune_to_msq(t, &b);
            put("msq", "mini_defaults.msq", b.data, b.len, 0);
            lt_buf_clear(&b);
            if (part) {
                lt_tune_to_msq(part, &b);
                put("msqpart", "demo.msqpart", b.data, b.len, -1);
                lt_tune_free(part);
            }
            lt_buf_free(&b);
            lt_tune_free(t);
        }
        if (ini) lt_ini_free(ini);
    }
    put_file("msqpart", "sample.msq", "tests/fixtures/tune/sample.msq", -1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: seedgen OUTDIR\n");
        return 2;
    }
    g_out = argv[1];
    logs();
    dashes();
    firmware();
    proto();
    tunes();
    return 0;
}
