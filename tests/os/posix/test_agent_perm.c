/* test_agent_perm.c - the assistant config (plaintext API key) and the MCP token are written
 * with mode 0600.
 *
 * PLATFORM FILE (tests/os/posix, built only for POSIX targets): includes sys/stat.h;
 * feature-test macros come from the build (tools/target.sh).
 */
#include "lt_test.h"
#include "../../../src/agent/agent.h"
#include "../../../src/plat/plat.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int mode_of(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int)(st.st_mode & 0777);
}

LT_TEST(private_files)
{
    char dir[96], t[65];
    char *p1, *p2;
    lt_ag_cfg c;
    lt_snprintf(dir, sizeof(dir), "build/test_agent_perm_%lu", (unsigned long)lt_plat_now_ms());
    lt_ag_cfg_defaults(&c);
    lt_strlcpy(c.api_key, "secret", sizeof(c.api_key));
    LT_CHECK(lt_ag_cfg_save(&c, dir) == 0);
    LT_CHECK(lt_ag_token(dir, 0, t) == 0);
    p1 = lt_path_join(dir, "assistant.conf");
    p2 = lt_path_join(dir, "mcp-token");
    LT_CHECK(mode_of(p1) == 0600);
    LT_CHECK(mode_of(p2) == 0600);
    /* saving again keeps the mode */
    LT_CHECK(lt_ag_cfg_save(&c, dir) == 0);
    LT_CHECK(mode_of(p1) == 0600);
    lt_free(p1);
    lt_free(p2);
}

int main(void)
{
    LT_RUN(private_files);
    LT_TEST_MAIN_END
}
