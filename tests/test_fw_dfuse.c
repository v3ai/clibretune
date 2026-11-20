/* test_fw_dfuse.c - the stepped flashing engines against simulated bootloaders: an STM32F407
 * ROM bootloader (DfuSe) and an OpenBLT XCP target, end to end on a virtual clock. The
 * simulated flash must equal the image afterwards while the settings sectors 10/11 (and,
 * over OpenBLT, the bootloader) keep their old bytes. */
#include <string.h>
#include "lt.h"
#include "fw/fw.h"
#include "lt_test.h"

static double vclock;
static char last_log[256];
static int n_warn_logs;

static double clk(void *u) { LT_UNUSED(u); return vclock; }

static void on_log(void *user, int level, const char *msg)
{
    LT_UNUSED(user);
    lt_strlcpy(last_log, msg, sizeof(last_log));
    if (level == 2) { n_warn_logs++; }
}

typedef struct rig {
    lt_fw_sim *sim;
    lt_fw_io io;
    lt_u8 *before;          /* flash content before the job */
    lt_u32 base, size;
    long steps, max_wait;   /* steps taken; longest single step gap asked for (ms) */
} rig;

static void rig_open(rig *r, int kind, const lt_fw_sim_opts *extra)
{
    lt_fw_sim_opts o;
    lt_u8 *fl;
    memset(&o, 0, sizeof(o));
    if (extra != NULL) { o = *extra; }
    o.kind = kind;
    if (o.erase_ms_16k == 0) { o.erase_ms_16k = 25; }
    if (o.write_ms == 0) { o.write_ms = 3; }
    memset(r, 0, sizeof(*r));
    vclock = 1000.0;
    n_warn_logs = 0;
    last_log[0] = '\0';
    r->sim = lt_fw_sim_new(&o, clk, NULL);
    lt_fw_sim_io(r->sim, &r->io);
    fl = lt_fw_sim_flash(r->sim, &r->base, &r->size);
    r->before = (lt_u8 *)lt_malloc(r->size);
    memcpy(r->before, fl, r->size);
}

static void rig_close(rig *r)
{
    lt_fw_sim_free(r->sim);
    lt_free(r->before);
}

/* run to the end; the clock jumps to the time the job asks for, plus a little per step */
static int run(rig *r, lt_fw_job *j, long limit)
{
    while (r->steps < limit) {
        double nx;
        r->steps++;
        if (!lt_fw_step(j, vclock)) { break; }
        nx = lt_fw_next_ms(j);
        if (nx > vclock) {
            if ((long)(nx - vclock) > r->max_wait) { r->max_wait = (long)(nx - vclock); }
            vclock = nx;
        } else {
            vclock += 0.2;
        }
    }
    {
        lt_fw_progress p;
        lt_fw_job_progress(j, &p);
        return p.phase;
    }
}

/* flash bytes in [a, a + n) equal the image (gaps: equal to the old content or erased) */
static int flash_matches(rig *r, const lt_fw_image *img)
{
    lt_u8 *fl = lt_fw_sim_flash(r->sim, NULL, NULL);
    size_t i;
    for (i = 0; i < img->n_segs; i++) {
        const lt_fw_seg *s = &img->segs[i];
        if (s->addr < r->base || s->addr - r->base + s->len > r->size) { return 0; }
        if (memcmp(fl + (s->addr - r->base), s->data, s->len) != 0) { return 0; }
    }
    return 1;
}

static int range_unchanged(rig *r, lt_u32 addr, lt_u32 len)
{
    lt_u8 *fl = lt_fw_sim_flash(r->sim, NULL, NULL);
    return memcmp(fl + (addr - r->base), r->before + (addr - r->base), len) == 0;
}

static lt_fw_opts miata_opts(void)
{
    lt_fw_opts o;
    const lt_fw_preset *p = lt_fw_preset_get(LT_FW_PRESET_MIATA);
    memset(&o, 0, sizeof(o));
    o.verify = 1;
    o.leave = 1;
    o.keep_settings = 1;
    o.protect = p->protect;
    o.n_protect = p->n_protect;
    o.app_base = p->app_base;
    return o;
}

LT_TEST(dfuse_end_to_end)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_job *j;
    lt_fw_progress p;
    rig_open(&r, LT_FW_SIM_DFUSE, NULL);
    lt_fw_sim_demo_image(&img, 1, 300);          /* merged OpenBLT + app, like rusefi_miata-ecu.bin */
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    LT_CHECK(j != NULL);
    LT_CHECK_INT(run(&r, j, 200000), LT_FW_PH_DONE);
    LT_CHECK(flash_matches(&r, &img));
    LT_CHECK(range_unchanged(&r, LT_FW_RUSEFI_CFG, LT_FW_RUSEFI_CFG_LEN));
    LT_CHECK_INT(lt_fw_sim_early_polls(r.sim), 0);       /* bwPollTimeout honoured */
    LT_CHECK_INT(lt_fw_sim_erases(r.sim), 7);            /* 4 x 16K + 64K + 2 x 128K */
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 1);
    LT_CHECK(r.max_wait >= 200);                         /* a 128K erase took its time */
    lt_fw_job_progress(j, &p);
    LT_CHECK_NEAR(p.frac, 1.0, 1e-9);
    LT_CHECK(strstr(p.msg, "Done") != NULL);
    LT_CHECK_INT(n_warn_logs, 0);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

/* odd sizes and gaps: short blocks, several address pointers; stay in DFU without verify */
LT_TEST(dfuse_odd_segments)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_job *j;
    lt_u8 d[5003];
    size_t i;
    for (i = 0; i < sizeof(d); i++) { d[i] = (lt_u8)(i * 7u + 3u); }
    rig_open(&r, LT_FW_SIM_DFUSE, NULL);
    lt_fw_image_init(&img);
    (void)lt_fw_image_add(&img, 0x08000000UL, d, 5003, NULL, 0);
    (void)lt_fw_image_add(&img, 0x08010003UL, d, 100, NULL, 0);
    (void)lt_fw_image_add(&img, 0x08004801UL, d + 7, 2049, NULL, 0);
    LT_CHECK_INT(lt_fw_image_finish(&img, NULL, 0), 0);
    o.leave = 0;
    o.verify = 1;
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 100000), LT_FW_PH_DONE);
    LT_CHECK(flash_matches(&r, &img));
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 0);
    LT_CHECK_INT(lt_fw_sim_erases(r.sim), 3);            /* sectors 0, 1 and 4 */
    LT_CHECK(range_unchanged(&r, 0x08008000UL, 0x8000));  /* sectors 2, 3 untouched */
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

LT_TEST(dfuse_recovers_from_stall_and_errors)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_sim_opts so;
    lt_fw_job *j;
    memset(&so, 0, sizeof(so));
    so.stall_dnload = 9;          /* during the erase */
    so.err_status = 20;           /* a program error on the 20th data block, once */
    so.err_repeat = 1;
    rig_open(&r, LT_FW_SIM_DFUSE, &so);
    lt_fw_sim_demo_image(&img, 1, 120);
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 200000), LT_FW_PH_DONE);
    LT_CHECK(flash_matches(&r, &img));
    LT_CHECK(range_unchanged(&r, LT_FW_RUSEFI_CFG, LT_FW_RUSEFI_CFG_LEN));
    LT_CHECK_INT(n_warn_logs, 2);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

LT_TEST(dfuse_gives_up_on_persistent_error)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_sim_opts so;
    lt_fw_job *j;
    lt_fw_progress p;
    memset(&so, 0, sizeof(so));
    so.err_status = 3;
    so.err_repeat = 50;
    rig_open(&r, LT_FW_SIM_DFUSE, &so);
    lt_fw_sim_demo_image(&img, 1, 40);
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 100000), LT_FW_PH_FAILED);
    lt_fw_job_progress(j, &p);
    LT_CHECK(strstr(p.msg, "retries") != NULL && strstr(p.msg, "errPROG") != NULL);
    LT_CHECK(r.steps < 2000);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

LT_TEST(dfuse_settings_guard)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_job *j;
    lt_fw_progress p;
    lt_u8 d[64];
    memset(d, 0x42, sizeof(d));
    rig_open(&r, LT_FW_SIM_DFUSE, NULL);
    lt_fw_sim_demo_image(&img, 1, 16);
    (void)lt_fw_image_add(&img, 0x080C0100UL, d, 64, NULL, 0);
    (void)lt_fw_image_finish(&img, NULL, 0);
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 1000), LT_FW_PH_FAILED);
    lt_fw_job_progress(j, &p);
    LT_CHECK(strstr(p.msg, "Keep ECU settings") != NULL);
    LT_CHECK_INT(lt_fw_sim_erases(r.sim), 0);
    LT_CHECK(range_unchanged(&r, r.base, r.size));
    lt_fw_job_free(j);
    /* the user unticked "keep settings": sector 10 is rewritten, 11 stays */
    o.keep_settings = 0;
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 100000), LT_FW_PH_DONE);
    LT_CHECK(flash_matches(&r, &img));
    LT_CHECK(!range_unchanged(&r, LT_FW_RUSEFI_CFG, 0x20000UL));
    LT_CHECK(range_unchanged(&r, LT_FW_RUSEFI_CFG + 0x20000UL, 0x20000UL));
    lt_fw_job_free(j);
    /* mass erase is refused while the settings are protected */
    o.keep_settings = 1;
    o.mass_erase = 1;
    rig_close(&r);
    rig_open(&r, LT_FW_SIM_DFUSE, NULL);
    lt_fw_image_free(&img);
    lt_fw_sim_demo_image(&img, 1, 16);
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 1000), LT_FW_PH_FAILED);
    LT_CHECK_INT(lt_fw_sim_erases(r.sim), 0);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

LT_TEST(dfuse_verify_catches_corruption)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_job *j;
    lt_fw_progress p;
    int poked = 0;
    rig_open(&r, LT_FW_SIM_DFUSE, NULL);
    lt_fw_sim_demo_image(&img, 1, 32);
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    while (lt_fw_step(j, vclock) && r.steps++ < 100000) {
        lt_fw_job_progress(j, &p);
        if (p.phase == LT_FW_PH_VERIFY && !poked) {
            lt_fw_sim_flash(r.sim, NULL, NULL)[0x9123] ^= 0x10;
            poked = 1;
        }
        vclock = lt_fw_next_ms(j) > vclock ? lt_fw_next_ms(j) : vclock + 0.2;
    }
    lt_fw_job_progress(j, &p);
    LT_CHECK_INT(p.phase, LT_FW_PH_FAILED);
    LT_CHECK(strstr(p.msg, "0x08009123") != NULL);
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 0);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

LT_TEST(dfuse_cancel)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_job *j;
    lt_fw_progress p;
    rig_open(&r, LT_FW_SIM_DFUSE, NULL);
    lt_fw_sim_demo_image(&img, 1, 64);
    j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);
    while (lt_fw_step(j, vclock) && r.steps++ < 100000) {
        lt_fw_job_progress(j, &p);
        if (p.phase == LT_FW_PH_WRITE && p.done > 8000) { lt_fw_cancel(j); }
        vclock = lt_fw_next_ms(j) > vclock ? lt_fw_next_ms(j) : vclock + 0.2;
    }
    lt_fw_job_progress(j, &p);
    LT_CHECK_INT(p.phase, LT_FW_PH_CANCELLED);
    LT_CHECK(strstr(p.msg, "incomplete") != NULL);
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 0);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

LT_TEST(openblt_end_to_end)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_sim_opts so;
    lt_fw_job *j;
    lt_fw_progress p;
    memset(&so, 0, sizeof(so));
    so.connect_ignore = 3;
    so.busy_ms = 1500;
    rig_open(&r, LT_FW_SIM_OPENBLT, &so);
    lt_fw_sim_demo_image(&img, 1, 200);
    j = lt_fw_job_new(LT_FW_OPENBLT, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 400000), LT_FW_PH_DONE);
    lt_fw_job_progress(j, &p);
    LT_CHECK(strstr(p.msg, "OpenBLT started") != NULL);
    LT_CHECK(range_unchanged(&r, r.base, 0x8000));            /* bootloader not rewritten */
    LT_CHECK(range_unchanged(&r, LT_FW_RUSEFI_CFG, LT_FW_RUSEFI_CFG_LEN));
    {
        lt_u8 *fl = lt_fw_sim_flash(r.sim, NULL, NULL);
        LT_CHECK(memcmp(fl + 0x8000, img.segs[0].data + 0x8000, 200u * 1024u) == 0);
    }
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 1);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

LT_TEST(openblt_refusals)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = miata_opts();
    lt_fw_sim_opts so;
    lt_fw_job *j;
    lt_fw_progress p;
    lt_u8 d[256];
    memset(&so, 0, sizeof(so));
    so.pgm_protected = 1;
    rig_open(&r, LT_FW_SIM_OPENBLT, &so);
    lt_fw_sim_demo_image(&img, 1, 16);
    j = lt_fw_job_new(LT_FW_OPENBLT, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 10000), LT_FW_PH_FAILED);
    lt_fw_job_progress(j, &p);
    LT_CHECK(strstr(p.msg, "seed/key") != NULL);
    LT_CHECK(range_unchanged(&r, r.base, r.size));
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
    /* only a bootloader: nothing to send */
    memset(d, 1, sizeof(d));
    lt_fw_image_init(&img);
    (void)lt_fw_image_add(&img, 0x08000000UL, d, sizeof(d), NULL, 0);
    (void)lt_fw_image_finish(&img, NULL, 0);
    rig_open(&r, LT_FW_SIM_OPENBLT, NULL);
    j = lt_fw_job_new(LT_FW_OPENBLT, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 100), LT_FW_PH_FAILED);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
    /* no answer at all */
    memset(&so, 0, sizeof(so));
    so.connect_ignore = 1000;
    rig_open(&r, LT_FW_SIM_OPENBLT, &so);
    lt_fw_sim_demo_image(&img, 1, 16);
    j = lt_fw_job_new(LT_FW_OPENBLT, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 1000000), LT_FW_PH_FAILED);
    lt_fw_job_progress(j, &p);
    LT_CHECK(strstr(p.msg, "does not answer") != NULL);
    lt_fw_job_free(j);
    lt_fw_image_free(&img);
    rig_close(&r);
}

/* the stub USB backend (or a host without devices) must not crash the scan */
LT_TEST(usb_scan_safe)
{
    lt_fw_found f[4];
    int n = lt_fw_scan(f, 4);
    LT_CHECK(n >= 0 || n == LT_USB_ENOTSUP || n == LT_USB_EACCES || n == LT_USB_EIO);
}

int main(void)
{
    LT_RUN(dfuse_end_to_end);
    LT_RUN(dfuse_odd_segments);
    LT_RUN(dfuse_recovers_from_stall_and_errors);
    LT_RUN(dfuse_gives_up_on_persistent_error);
    LT_RUN(dfuse_settings_guard);
    LT_RUN(dfuse_verify_catches_corruption);
    LT_RUN(dfuse_cancel);
    LT_RUN(openblt_end_to_end);
    LT_RUN(openblt_refusals);
    LT_RUN(usb_scan_safe);
    LT_TEST_MAIN_END
}
