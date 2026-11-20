/* fuzz_fw.c - firmware images (raw .bin, Intel HEX, S-record, DfuSe .dfu): parse with the format
 * picked by the first byte (0 = detect), then summary, facts, reads and the writers. */
#include "fuzz_common.h"
#include "fw/fw.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    lt_fw_image img;
    char err[200], sum[200];
    int fmt;
    if (size < 1) return 0;
    fmt = data[0] % 5;
    lt_fw_image_init(&img);
    if (lt_fw_image_parse(&img, data + 1, size - 1, fmt, LT_FW_STM32_FLASH, err, sizeof err) == 0) {
        lt_fw_facts f;
        lt_u8 out[256];
        lt_buf b;
        lt_fw_image_summary(&img, sum, sizeof sum);
        lt_fw_image_facts(&img, &f);
        if (img.n_segs > 0) {
            (void)lt_fw_image_read(&img, img.lo, out, sizeof out, 0xFF);
            (void)lt_fw_image_read(&img, img.last - 10u, out, sizeof out, 0xFF);
            (void)lt_fw_image_touches(&img, img.lo, 1);
            (void)lt_fw_image_touches(&img, 0xFFFFFF00UL, 0x100);
        }
        if (img.goal < 4u * 1024u * 1024u) {
            lt_buf_init(&b);
            lt_fw_write_ihex(&img, &b);
            lt_buf_clear(&b);
            lt_fw_write_srec(&img, &b);
            lt_buf_clear(&b);
            lt_fw_write_dfuse(&img, 0x0483, 0xDF11, 0, "Internal Flash", &b);
            lt_buf_free(&b);
        }
    }
    lt_fw_image_free(&img);
    return 0;
}
