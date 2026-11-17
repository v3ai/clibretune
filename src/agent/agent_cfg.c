/* agent_cfg.c - assistant settings file, API key lookup, MCP token.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "../project/project.h"
#include "../plat/plat.h"

int lt_ag_tier_parse(const char *s)
{
    if (s == NULL) { return LT_AG_TIER_READ; }
    if (strcmp(s, "tune") == 0) { return LT_AG_TIER_TUNE; }
    if (strcmp(s, "config") == 0) { return LT_AG_TIER_CONFIG; }
    return LT_AG_TIER_READ;          /* unknown values never widen the tier */
}

const char *lt_ag_tier_name(int tier)
{
    if (tier == LT_AG_TIER_TUNE) { return "tune"; }
    if (tier == LT_AG_TIER_CONFIG) { return "config"; }
    return "read";
}

void lt_ag_cfg_defaults(lt_ag_cfg *c)
{
    memset(c, 0, sizeof(*c));
    c->tier = LT_AG_TIER_READ;
    lt_strlcpy(c->base_url, "http://127.0.0.1:11434/v1", sizeof(c->base_url));
    lt_strlcpy(c->model, "llama3.1", sizeof(c->model));
    c->mcp_port = 8765;
}

static char *conf_path(const char *dir) { return lt_path_join(dir, "assistant.conf"); }

int lt_ag_cfg_load(lt_ag_cfg *c, const char *dir)
{
    lt_props p;
    char *path;
    const char *v;
    int n = 0;
    lt_bool b = LT_FALSE;
    lt_ag_cfg_defaults(c);
    if (dir == NULL) { return -1; }
    path = conf_path(dir);
    lt_props_init(&p);
    if (!lt_file_exists(path) || lt_props_load(&p, path) != 0) {
        lt_props_free(&p);
        lt_free(path);
        return -1;
    }
    if (lt_props_get_bool(&p, "enabled", &b)) { c->enabled = b ? 1 : 0; }
    if (lt_props_get_bool(&p, "ack", &b)) { c->ack = b ? 1 : 0; }
    if (lt_props_get_bool(&p, "mcp", &b)) { c->mcp_on = b ? 1 : 0; }
    if (lt_props_get_i32(&p, "mcp_port", &n) && n >= 1024 && n <= 65535) { c->mcp_port = n; }
    c->tier = lt_ag_tier_parse(lt_props_get(&p, "tier"));
    v = lt_props_get(&p, "base_url");
    if (v != NULL && v[0] != '\0') { lt_strlcpy(c->base_url, v, sizeof(c->base_url)); }
    v = lt_props_get(&p, "model");
    if (v != NULL && v[0] != '\0') { lt_strlcpy(c->model, v, sizeof(c->model)); }
    v = lt_props_get(&p, "api_key");
    if (v != NULL) { lt_strlcpy(c->api_key, v, sizeof(c->api_key)); }
    lt_props_free(&p);
    lt_free(path);
    return 0;
}

/* write a file that only the owner can read: create it empty, restrict it, then fill it */
static int write_private(const char *path, const char *text, size_t len)
{
    if (lt_file_write(path, "", 0) != 0) { return -1; }
    (void)lt_plat_chmod_private(path);
    return lt_file_write(path, text, len);
}

int lt_ag_cfg_save(const lt_ag_cfg *c, const char *dir)
{
    lt_props p;
    char *path, *text;
    int rc;
    if (dir == NULL) { return -1; }
    lt_props_init(&p);
    lt_props_set(&p, "enabled", c->enabled ? "true" : "false");
    lt_props_set(&p, "ack", c->ack ? "true" : "false");
    lt_props_set(&p, "tier", lt_ag_tier_name(c->tier));
    lt_props_set(&p, "base_url", c->base_url);
    lt_props_set(&p, "model", c->model);
    lt_props_set(&p, "api_key", c->api_key);
    lt_props_set(&p, "mcp", c->mcp_on ? "true" : "false");
    {
        char b[16];
        lt_snprintf(b, sizeof(b), "%d", c->mcp_port);
        lt_props_set(&p, "mcp_port", b);
    }
    (void)lt_plat_mkdirs(dir);
    path = conf_path(dir);
    text = lt_props_to_string(&p);
    rc = write_private(path, text, strlen(text));
    lt_free(text);
    lt_free(path);
    lt_props_free(&p);
    return rc;
}

size_t lt_ag_cfg_key(const lt_ag_cfg *c, char *out, size_t cap)
{
    char *e = lt_plat_getenv("LT_AI_KEY");
    size_t n;
    if (e != NULL && e[0] != '\0') {
        n = lt_strlcpy(out, e, cap);
        lt_free(e);
        return n;
    }
    lt_free(e);
    return lt_strlcpy(out, c->api_key, cap);
}

int lt_ag_token(const char *dir, int regenerate, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    char *path = lt_path_join(dir, "mcp-token");
    char *txt;
    size_t len = 0, i;
    lt_u8 rnd[32];
    out[0] = '\0';
    if (!regenerate && lt_file_exists(path)) {
        txt = lt_file_read(path, &len);
        if (txt != NULL && len >= 64) {
            memcpy(out, txt, 64);
            out[64] = '\0';
            lt_free(txt);
            lt_free(path);
            return 0;
        }
        lt_free(txt);
    }
    if (lt_plat_random(rnd, sizeof(rnd)) != 0) {
        lt_err_set("no secure random source for the MCP token");
        lt_free(path);
        return -1;
    }
    for (i = 0; i < 32; i++) {
        out[2 * i] = hex[rnd[i] >> 4];
        out[2 * i + 1] = hex[rnd[i] & 15];
    }
    out[64] = '\0';
    (void)lt_plat_mkdirs(dir);
    if (write_private(path, out, 64) != 0) { lt_free(path); return -1; }
    lt_free(path);
    return 0;
}
