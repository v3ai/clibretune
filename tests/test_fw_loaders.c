/* test_fw_loaders.c - the serial and HID flashing engines against simulated bootloaders, end to
 * end on a virtual clock: STK500v2 (Arduino Mega 2560), Teensy HalfKay (3.6 and 4.1), OpenBLT over
 * a UART and over CAN through an SLCAN adapter; plus the presets, the device fit check and the
 * serial adapter's error paths. */
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
    lt_u8 *before;
    lt_u32 base, size;
    long steps;
} rig;

static void rig_open(rig *r, int kind, const lt_fw_sim_opts *extra)
{
    lt_fw_sim_opts o;
    lt_u8 *fl;
    memset(&o, 0, sizeof(o));
    if (extra != NULL) { o = *extra; }
    o.kind = kind;
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

static int phase_of(lt_fw_job *j)
{
    lt_fw_progress p;
    lt_fw_job_progress(j, &p);
    return p.phase;
}

/* run until the end (or until phase `stop` when >= 0) */
static int run_to(rig *r, lt_fw_job *j, long limit, int stop)
{
    while (r->steps < limit) {
        double nx;
        if (stop >= 0 && phase_of(j) == stop) { break; }
        r->steps++;
        if (!lt_fw_step(j, vclock)) { break; }
        nx = lt_fw_next_ms(j);
        vclock = nx > vclock ? nx : vclock + 0.2;
    }
    return phase_of(j);
}

static int run(rig *r, lt_fw_job *j, long limit) { return run_to(r, j, limit, -1); }

static const char *msg_of(lt_fw_job *j)
{
    static lt_fw_progress p;
    lt_fw_job_progress(j, &p);
    return p.msg;
}

static lt_fw_opts preset_opts(int preset)
{
    lt_fw_opts o;
    const lt_fw_preset *p = lt_fw_preset_get(preset);
    memset(&o, 0, sizeof(o));
    o.verify = 1;
    o.leave = 1;
    o.app_base = p->app_base;
    o.dev_base = p->dev_base;
    o.dev_size = p->dev_size;
    return o;
}

/* device flash = the image, in every page/block the image touches gaps read as 0xFF */
static int flash_has_image(rig *r, const lt_fw_image *img, lt_u32 dev_base, lt_u32 page)
{
    lt_u8 *fl = lt_fw_sim_flash(r->sim, NULL, NULL);
    size_t i;
    for (i = 0; i < img->n_segs; i++) {
        const lt_fw_seg *s = &img->segs[i];
        lt_u32 a = s->addr - dev_base, pa;
        if (memcmp(fl + a, s->data, s->len) != 0) { return 0; }
        for (pa = a / page * page; pa < a; pa++) {
            if (!lt_fw_image_touches(img, dev_base + pa, 1) && fl[pa] != 0xFF) { return 0; }
        }
    }
    return 1;
}

static int unchanged(rig *r, lt_u32 off, lt_u32 len)
{
    lt_u8 *fl = lt_fw_sim_flash(r->sim, NULL, NULL);
    return memcmp(fl + off, r->before + off, len) == 0;
}

/* ------------------------------------------------------------------ STK500v2 */
LT_TEST(stk500_end_to_end)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_SPEEDUINO_MEGA);
    lt_fw_sim_opts so;
    lt_fw_job *j;
    lt_u32 pages;
    memset(&so, 0, sizeof(so));
    so.noise = 1;                                   /* junk before the first answer */
    rig_open(&r, LT_FW_SIM_STK500, &so);
    lt_fw_sim_avr_image(&img, 60);
    pages = (img.segs[0].len + 255u) / 256u + 1u;   /* + the small block in its own page */
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &img, &o, on_log, NULL);
    LT_CHECK(j != NULL);
    LT_CHECK_INT(run(&r, j, 400000), LT_FW_PH_DONE);
    LT_CHECK(strstr(msg_of(j), "started the new firmware") != NULL);
    LT_CHECK(flash_has_image(&r, &img, 0, 256));
    LT_CHECK_INT(lt_fw_sim_writes(r.sim), (long)pages);
    LT_CHECK_INT(lt_fw_sim_resets(r.sim), 1);
    LT_CHECK(unchanged(&r, LT_FW_MEGA2560_APP, 0x2000));      /* the boot section */
    LT_CHECK(unchanged(&r, 0x20000, 0x1000));                  /* past the image */
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 1);
    lt_fw_job_free(j);
    rig_close(&r);
    /* no verify, no start: still written, the bootloader is left alone */
    rig_open(&r, LT_FW_SIM_STK500, NULL);
    o.verify = 0;
    o.leave = 0;
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 400000), LT_FW_PH_DONE);
    LT_CHECK(flash_has_image(&r, &img, 0, 256));
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 0);
    lt_fw_job_free(j);
    rig_close(&r);
    lt_fw_image_free(&img);
}

LT_TEST(stk500_refusals)
{
    rig r;
    lt_fw_image img, big;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_SPEEDUINO_MEGA);
    lt_fw_sim_opts so;
    lt_fw_job *j;
    lt_u8 d[64];
    lt_fw_sim_avr_image(&img, 8);
    /* an ATmega1280: nothing written */
    memset(&so, 0, sizeof(so));
    so.bad_sig = 1;
    rig_open(&r, LT_FW_SIM_STK500, &so);
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 100000), LT_FW_PH_FAILED);
    LT_CHECK(strstr(msg_of(j), "not an ATmega2560") != NULL);
    LT_CHECK(unchanged(&r, 0, r.size));
    lt_fw_job_free(j);
    rig_close(&r);
    /* nobody answers (an OpenBLT UART device on the port): gives up after the retries, resets twice */
    rig_open(&r, LT_FW_SIM_OPENBLT_UART, NULL);
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 1000000), LT_FW_PH_FAILED);
    LT_CHECK(strstr(msg_of(j), "does not answer") != NULL);
    lt_fw_job_free(j);
    rig_close(&r);
    /* an image with the bootloader in it (Arduino "with_bootloader.hex") */
    memset(d, 0x11, sizeof(d));
    lt_fw_image_init(&big);
    (void)lt_fw_image_add(&big, 0, d, sizeof(d), NULL, 0);
    (void)lt_fw_image_add(&big, 0x3E000UL, d, sizeof(d), NULL, 0);
    (void)lt_fw_image_finish(&big, NULL, 0);
    rig_open(&r, LT_FW_SIM_STK500, NULL);
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &big, &o, on_log, NULL);
    LT_CHECK_INT(phase_of(j), LT_FW_PH_FAILED);
    LT_CHECK(strstr(msg_of(j), "bootloader") != NULL);
    lt_fw_job_free(j);
    /* a USB transport cannot carry it */
    {
        lt_fw_sim_opts uo;
        lt_fw_sim *us;
        lt_fw_io uio;
        memset(&uo, 0, sizeof(uo));
        uo.kind = LT_FW_SIM_DFUSE;
        us = lt_fw_sim_new(&uo, clk, NULL);
        lt_fw_sim_io(us, &uio);
        j = lt_fw_job_new(LT_FW_STK500, &uio, &img, &o, on_log, NULL);
        LT_CHECK_INT(phase_of(j), LT_FW_PH_FAILED);
        lt_fw_job_free(j);
        j = lt_fw_job_new(LT_FW_DFUSE, &r.io, &img, &o, on_log, NULL);   /* and DFU needs USB */
        LT_CHECK_INT(phase_of(j), LT_FW_PH_FAILED);
        lt_fw_job_free(j);
        lt_fw_sim_free(us);
    }
    rig_close(&r);
    lt_fw_image_free(&big);
    lt_fw_image_free(&img);
}

LT_TEST(stk500_without_modem_lines)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_SPEEDUINO_MEGA);
    lt_fw_sim_opts so;
    lt_fw_job *j;
    memset(&so, 0, sizeof(so));
    so.no_lines = 1;                                /* the user pressed reset */
    rig_open(&r, LT_FW_SIM_STK500, &so);
    lt_fw_sim_avr_image(&img, 4);
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 200000), LT_FW_PH_DONE);
    LT_CHECK(n_warn_logs >= 1);
    LT_CHECK_INT(lt_fw_sim_resets(r.sim), 0);
    LT_CHECK(flash_has_image(&r, &img, 0, 256));
    lt_fw_job_free(j);
    rig_close(&r);
    lt_fw_image_free(&img);
}

LT_TEST(stk500_verify_and_cancel)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_SPEEDUINO_MEGA);
    lt_fw_job *j;
    lt_u8 *fl;
    lt_fw_sim_avr_image(&img, 20);
    /* a byte that does not stick: verify must catch it */
    rig_open(&r, LT_FW_SIM_STK500, NULL);
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run_to(&r, j, 400000, LT_FW_PH_VERIFY), LT_FW_PH_VERIFY);
    fl = lt_fw_sim_flash(r.sim, NULL, NULL);
    fl[0x1234] ^= 0x10;
    LT_CHECK_INT(run(&r, j, 400000), LT_FW_PH_FAILED);
    LT_CHECK(strstr(msg_of(j), "Verify failed at 0x01234") != NULL);
    lt_fw_job_free(j);
    rig_close(&r);
    /* cancel in the middle of writing */
    rig_open(&r, LT_FW_SIM_STK500, NULL);
    j = lt_fw_job_new(LT_FW_STK500, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run_to(&r, j, 400000, LT_FW_PH_WRITE), LT_FW_PH_WRITE);
    while (lt_fw_sim_writes(r.sim) < 5) { (void)lt_fw_step(j, vclock); vclock += 1.0; }
    lt_fw_cancel(j);
    LT_CHECK_INT(run(&r, j, 1000), LT_FW_PH_CANCELLED);
    LT_CHECK(strstr(msg_of(j), "incomplete") != NULL);
    lt_fw_job_free(j);
    rig_close(&r);
    lt_fw_image_free(&img);
}

/* ------------------------------------------------------------------ HalfKay */
LT_TEST(halfkay_teensy41)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_TEENSY41);
    lt_fw_sim_opts so;
    lt_fw_job *j;
    lt_fw_progress p;
    lt_u8 *fl;
    memset(&so, 0, sizeof(so));
    so.teensy4 = 1;
    so.erase_ms = 20000;                            /* needs PJRC's long first-block timeout */
    rig_open(&r, LT_FW_SIM_HALFKAY, &so);
    LT_CHECK(r.size == 8126464UL);
    lt_fw_sim_teensy_image(&img, 1, 100);
    LT_CHECK(img.lo == LT_FW_TEENSY4_FLASH);
    j = lt_fw_job_new(LT_FW_HALFKAY, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 100000), LT_FW_PH_DONE);
    lt_fw_job_progress(j, &p);
    LT_CHECK(strstr(p.msg, "rebooted") != NULL);
    LT_CHECK(flash_has_image(&r, &img, LT_FW_TEENSY4_FLASH, 1024));
    /* 100 blocks touched by the main part, two of them blank (skipped), + 1 for the small tail block */
    LT_CHECK_INT(lt_fw_sim_writes(r.sim), 99);
    LT_CHECK_INT(lt_fw_sim_early_polls(r.sim), 0);  /* no erase timed out */
    LT_CHECK_INT(lt_fw_sim_erases(r.sim), 1);
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 1);
    fl = lt_fw_sim_flash(r.sim, NULL, NULL);
    LT_CHECK(fl[0x2000] == 0xFF && fl[0x27FF] == 0xFF);   /* the blank blocks stay erased */
    LT_CHECK(fl[200u * 1024u] == 0xFF);                 /* the rest of the chip is erased */
    lt_fw_job_free(j);
    rig_close(&r);
    lt_fw_image_free(&img);
}

LT_TEST(halfkay_teensy36_and_mismatch)
{
    rig r;
    lt_fw_image img, t4;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_TEENSY36);
    lt_fw_job *j;
    lt_fw_sim_teensy_image(&img, 0, 64);
    lt_fw_sim_teensy_image(&t4, 1, 64);
    rig_open(&r, LT_FW_SIM_HALFKAY, NULL);
    LT_CHECK(r.size == 1048576UL);
    j = lt_fw_job_new(LT_FW_HALFKAY, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 100000), LT_FW_PH_DONE);
    LT_CHECK(flash_has_image(&r, &img, 0, 1024));
    lt_fw_job_free(j);
    /* a Teensy 4 image on a Teensy 3.6 and the reverse: refused before anything is sent */
    j = lt_fw_job_new(LT_FW_HALFKAY, &r.io, &t4, &o, on_log, NULL);
    LT_CHECK_INT(phase_of(j), LT_FW_PH_FAILED);
    LT_CHECK(strstr(msg_of(j), "Teensy 4.x image") != NULL);
    lt_fw_job_free(j);
    o = preset_opts(LT_FW_PRESET_TEENSY41);
    j = lt_fw_job_new(LT_FW_HALFKAY, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(phase_of(j), LT_FW_PH_FAILED);
    LT_CHECK(strstr(msg_of(j), "Teensy 3.x") != NULL);
    lt_fw_job_free(j);
    o = preset_opts(LT_FW_PRESET_TEENSY35);         /* 512 KB: a 64 KB image fits */
    LT_CHECK_INT(lt_fw_image_fits(&img, o.dev_base, o.dev_size, NULL, 0), 0);
    rig_close(&r);
    lt_fw_image_free(&img);
    lt_fw_image_free(&t4);
}

/* ------------------------------------------------------------------ OpenBLT UART / CAN */
LT_TEST(openblt_uart_end_to_end)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_OPENBLT);
    lt_fw_sim_opts so;
    lt_fw_job *j;
    memset(&so, 0, sizeof(so));
    so.connect_ignore = 2;
    so.busy_ms = 800;
    rig_open(&r, LT_FW_SIM_OPENBLT_UART, &so);
    lt_fw_sim_demo_image(&img, 0, 48);              /* an app at 0x08008000 */
    j = lt_fw_job_new(LT_FW_OPENBLT_UART, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 400000), LT_FW_PH_DONE);
    LT_CHECK(strstr(msg_of(j), "OpenBLT started") != NULL);
    LT_CHECK(memcmp(lt_fw_sim_flash(r.sim, NULL, NULL) + 0x8000, img.segs[0].data, img.segs[0].len) == 0);
    LT_CHECK(unchanged(&r, 0, 0x8000));
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 1);
    lt_fw_job_free(j);
    rig_close(&r);
    lt_fw_image_free(&img);
}

LT_TEST(openblt_can_end_to_end)
{
    rig r;
    lt_fw_image img;
    lt_fw_opts o = preset_opts(LT_FW_PRESET_OPENBLT);
    lt_fw_sim_opts so;
    lt_fw_job *j;
    lt_fw_sim_demo_image(&img, 0, 12);
    /* defaults: 500 kbit/s, 0x667 / 0x7E1 */
    rig_open(&r, LT_FW_SIM_OPENBLT_CAN, NULL);
    j = lt_fw_job_new(LT_FW_OPENBLT_CAN, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 800000), LT_FW_PH_DONE);
    LT_CHECK(memcmp(lt_fw_sim_flash(r.sim, NULL, NULL) + 0x8000, img.segs[0].data, img.segs[0].len) == 0);
    LT_CHECK_INT(lt_fw_sim_left(r.sim), 1);
    lt_fw_job_free(j);
    rig_close(&r);
    /* 29-bit ids at 250 kbit/s on both sides */
    memset(&so, 0, sizeof(so));
    so.can_kbps = 250;
    so.can_tx = 0x18DA00F1UL | LT_FW_CAN_EXT;
    so.can_rx = 0x18DAF100UL | LT_FW_CAN_EXT;
    rig_open(&r, LT_FW_SIM_OPENBLT_CAN, &so);
    o.can_kbps = 250;
    o.can_tx = so.can_tx;
    o.can_rx = so.can_rx;
    j = lt_fw_job_new(LT_FW_OPENBLT_CAN, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 800000), LT_FW_PH_DONE);
    lt_fw_job_free(j);
    rig_close(&r);
    /* the wrong bit rate: no answer */
    rig_open(&r, LT_FW_SIM_OPENBLT_CAN, NULL);
    j = lt_fw_job_new(LT_FW_OPENBLT_CAN, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(run(&r, j, 800000), LT_FW_PH_FAILED);
    LT_CHECK(strstr(msg_of(j), "does not answer on CAN id") != NULL);
    LT_CHECK(unchanged(&r, 0, r.size));
    lt_fw_job_free(j);
    /* a bit rate SLCAN has no code for */
    o.can_kbps = 333;
    j = lt_fw_job_new(LT_FW_OPENBLT_CAN, &r.io, &img, &o, on_log, NULL);
    LT_CHECK_INT(phase_of(j), LT_FW_PH_FAILED);
    lt_fw_job_free(j);
    rig_close(&r);
    lt_fw_image_free(&img);
}

/* ------------------------------------------------------------------ helpers */
LT_TEST(presets_and_helpers)
{
    const lt_fw_preset *m = lt_fw_preset_get(LT_FW_PRESET_SPEEDUINO_MEGA);
    lt_fw_io io;
    char err[200];
    int i;
    LT_CHECK_INT(lt_fw_preset_for_signature("speeduino 202501"), LT_FW_PRESET_SPEEDUINO_MEGA);
    LT_CHECK_STR(m->name, "Speeduino (Arduino Mega 2560)");
    LT_CHECK(m->methods == (1 << LT_FW_STK500) && m->dev_size == 0x3E000UL);
    LT_CHECK(lt_fw_preset_get(LT_FW_PRESET_TEENSY41)->dev_base == 0x60000000UL);
    LT_CHECK(lt_fw_preset_get(LT_FW_PRESET_TEENSY35)->dev_size == 524288UL);
    LT_CHECK((lt_fw_preset_get(LT_FW_PRESET_OPENBLT)->methods & (1 << LT_FW_OPENBLT_CAN)) != 0);
    for (i = 0; i < LT_FW_PRESET_COUNT; i++) {
        LT_CHECK(lt_fw_preset_get(i)->methods != 0 && lt_fw_preset_get(i)->name != NULL);
    }
    for (i = 0; i < LT_FW_METHOD_COUNT; i++) { LT_CHECK(strcmp(lt_fw_method_name(i), "?") != 0); }
    LT_CHECK(lt_fw_method_serial(LT_FW_STK500) && lt_fw_method_serial(LT_FW_OPENBLT_CAN));
    LT_CHECK(!lt_fw_method_serial(LT_FW_HALFKAY) && !lt_fw_method_verifies(LT_FW_HALFKAY));
    LT_CHECK_INT(lt_fw_slcan_speed(500), 6);
    LT_CHECK_INT(lt_fw_slcan_speed(10), 0);
    LT_CHECK_INT(lt_fw_slcan_speed(1000), 8);
    LT_CHECK_INT(lt_fw_slcan_speed(333), -1);
    LT_CHECK(strstr(lt_fw_udev_rule, "16c0") != NULL && strstr(lt_fw_udev_rule, "0478") != NULL);
    LT_CHECK(strstr(lt_fw_udev_rule, "60ac") != NULL && strstr(lt_fw_udev_rule, LT_FW_UDEV_FILE) != NULL);
    /* the serial adapter: a port that does not exist */
    err[0] = '\0';
    LT_CHECK(lt_fw_serial_open("/nonexistent/ttyFW0", 115200L, &io, err, sizeof(err)) == NULL);
    LT_CHECK(err[0] != '\0');
    LT_CHECK(lt_fw_serial_open("", 115200L, &io, err, sizeof(err)) == NULL);
    LT_CHECK(lt_fw_serial_hint("cannot open /dev/ttyACM0: Permission denied", 0) != NULL);
    LT_CHECK(strstr(lt_fw_serial_hint("cannot open /dev/ttyACM0: Permission denied", 0), "dialout") != NULL);
    LT_CHECK(lt_fw_serial_hint("something else", 0) == NULL);
    lt_fw_serial_close(NULL);
}

int main(void)
{
    LT_RUN(stk500_end_to_end);
    LT_RUN(stk500_refusals);
    LT_RUN(stk500_without_modem_lines);
    LT_RUN(stk500_verify_and_cancel);
    LT_RUN(halfkay_teensy41);
    LT_RUN(halfkay_teensy36_and_mismatch);
    LT_RUN(openblt_uart_end_to_end);
    LT_RUN(openblt_can_end_to_end);
    LT_RUN(presets_and_helpers);
    LT_TEST_MAIN_END
}
