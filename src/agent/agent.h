/* agent.h - the assistant (G-123, local-only form) and its tool layer, shared with the MCP
 * server (src/mcp).  Everything here is UI-free:
 *   config    <config>/assistant.conf (0600; the API key is stored in PLAINTEXT, like the
 *             original app's v1 setting; the environment variable LT_AI_KEY overrides it)
 *   tools     the read / propose tool catalogue and its executor over an lt_ag_env
 *   proposals writes are never applied by a tool: they queue as proposals that the UI
 *             shows for explicit confirmation, validated against the INI and clamped to
 *             the AutoTune authority limits
 *   chat      OpenAI-compatible /chat/completions client (SSE streaming or plain JSON),
 *             tool-calling loop, stepped once per frame over src/net/http.
 * Only plain http:// endpoints are possible (no TLS): Ollama, LM Studio, llama.cpp, or a
 * local proxy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_AGENT_H
#define LT_AGENT_H

#include "../lt.h"
#include "../ini/ini.h"
#include "../json/json.h"
#include "../net/http.h"

LT_BEGIN_DECLS

/* ---- capability tiers (cumulative) ---- */
enum { LT_AG_TIER_READ = 0, LT_AG_TIER_TUNE = 1, LT_AG_TIER_CONFIG = 2 };
int  lt_ag_tier_parse(const char *s);          /* unknown -> read */
const char *lt_ag_tier_name(int tier);

/* ---- configuration ---- */
typedef struct lt_ag_cfg {
    int  enabled;
    int  ack;                    /* risk acknowledgement */
    int  tier;
    char base_url[200];          /* e.g. http://127.0.0.1:11434/v1 */
    char model[96];
    char api_key[200];           /* plaintext in assistant.conf unless LT_AI_KEY is set */
    int  mcp_on;                 /* HTTP MCP server on 127.0.0.1 */
    int  mcp_port;
} lt_ag_cfg;
void lt_ag_cfg_defaults(lt_ag_cfg *c);
int  lt_ag_cfg_load(lt_ag_cfg *c, const char *dir);
int  lt_ag_cfg_save(const lt_ag_cfg *c, const char *dir);    /* file mode 0600 */
/* key in effect (LT_AI_KEY first); returns its length */
size_t lt_ag_cfg_key(const lt_ag_cfg *c, char *out, size_t cap);
/* the MCP bearer token (64 hex chars + NUL) in <dir>/mcp-token (0600); regenerate = new one */
int  lt_ag_token(const char *dir, int regenerate, char out[65]);

/* ---- tool environment ---- */
typedef struct lt_ag_env {
    const lt_ini *ini;           /* NULL: no definition loaded */
    lt_pagesrc    src;           /* page images (ECU shadow or tune) */
    lt_ini_rt    *rt;            /* realtime values, NULL when offline */
    const char   *log_dir;       /* datalogs folder or NULL */
} lt_ag_env;

/* ---- proposals ---- */
enum { LT_AG_P_CELL = 0, LT_AG_P_BULK, LT_AG_P_CONST };
enum { LT_AG_PENDING = 0, LT_AG_ACCEPTED, LT_AG_REJECTED };
typedef struct lt_ag_prop {
    int    kind;
    char   name[64];             /* table or constant */
    size_t x, y;                 /* CELL: column, row */
    char   op[16];               /* BULK: scale|offset|set_equal|smooth|interpolate */
    double param;                /* BULK parameter; CELL/CONST new value */
    size_t *cells;               /* BULK: pairs x,y (2 * n_cells) */
    size_t n_cells;
    double old_value;
    double clamped_from;
    int    was_clamped;
    int    valid;                /* validation passed */
    int    dangerous;            /* pin / trigger / limit style constants */
    int    state;                /* LT_AG_PENDING/ACCEPTED/REJECTED */
    int    applied;
    char   reason[200];          /* the model's reasoning */
    char   note[200];            /* validation / clamp notice */
} lt_ag_prop;

typedef struct lt_ag_props { lt_ag_prop *v; size_t n, cap; } lt_ag_props;
void lt_ag_props_init(lt_ag_props *p);
void lt_ag_props_free(lt_ag_props *p);
void lt_ag_props_clear(lt_ag_props *p);
/* one-line description for the review queue */
void lt_ag_prop_describe(const lt_ag_prop *p, char *out, size_t cap);
/* write every accepted, valid, not yet applied proposal into dst (marks them applied); the
 * caller creates the restore point first and calls lt_app_edited afterwards.  Returns the
 * number applied, or negative on a write error. */
int  lt_ag_props_apply(lt_ag_props *p, const lt_ini *ini, lt_pagesrc *dst);
size_t lt_ag_props_pending(const lt_ag_props *p);
size_t lt_ag_props_accepted(const lt_ag_props *p);

/* ---- tools ---- */
typedef struct lt_ag_tool {
    const char *name;
    const char *desc;
    const char *schema;          /* JSON schema of the arguments */
    int         min_tier;        /* LT_AG_TIER_*; read tools are 0 */
    int         write;           /* propose_* tool */
} lt_ag_tool;
size_t lt_ag_tool_count(void);
const lt_ag_tool *lt_ag_tool_at(size_t i);
const lt_ag_tool *lt_ag_tool_find(const char *name);
int  lt_ag_tool_allowed(int tier, const char *name);
/* catalogue as JSON.  openai: [{"type":"function","function":{name,description,parameters}}],
 * else MCP: [{name,description,inputSchema}].  tier filters; read_only drops propose tools. */
lt_json *lt_ag_catalogue(int tier, int read_only, int openai);
/* run one tool.  args may be NULL.  props NULL = proposals are refused (MCP).  out receives a
 * JSON document; on failure {"error": "..."} and the return value is -1. */
int  lt_ag_tool_run(const lt_ag_env *env, int tier, const char *name, const lt_json *args,
                    lt_ag_props *props, lt_buf *out);

/* ---- chat ---- */
#define LT_AG_MAX_ROUNDS 6
#define LT_AG_MAX_CALLS 8
typedef struct lt_ag_call { char id[64]; char name[48]; lt_buf args; } lt_ag_call;
typedef struct lt_ag_chat {
    lt_json   *msgs;             /* OpenAI messages (user/assistant/tool), no system prompt */
    lt_http   *http;
    int        busy;
    int        round;
    int        sse;              /* current response is an event stream */
    int        cancelled;
    char       status[96];       /* "thinking (round 2)...", "reading veTable1..." */
    char       error[200];
    char       context[160];     /* extra line for the system prompt (open table ...) */
    lt_buf     reply;            /* assistant text so far (streamed) */
    lt_buf     raw;              /* non-stream body / error body */
    lt_buf     sse_line;
    lt_ag_call calls[LT_AG_MAX_CALLS];
    int        n_calls;
    long       n_tok;
    lt_ag_props props;
    unsigned long changes;       /* bumps whenever the transcript changed (redraw / scroll) */
} lt_ag_chat;
void lt_ag_chat_init(lt_ag_chat *c);
void lt_ag_chat_free(lt_ag_chat *c);
void lt_ag_chat_reset(lt_ag_chat *c);                 /* new conversation (also clears proposals) */
void lt_ag_chat_cancel(lt_ag_chat *c);
/* append a user message and start the request; 0 ok, negative with c->error set */
int  lt_ag_chat_send(lt_ag_chat *c, const lt_ag_cfg *cfg, const char *text);
/* advance (never blocks): runs finished tool calls and starts the next round.
 * Returns 1 when the transcript changed. */
int  lt_ag_chat_step(lt_ag_chat *c, const lt_ag_cfg *cfg, const lt_ag_env *env);
/* request JSON for the current history (tests, and the request actually sent) */
char *lt_ag_chat_request(const lt_ag_chat *c, const lt_ag_cfg *cfg, int stream);
/* feed response body bytes / parse a whole non-stream body (tests use these directly) */
void lt_ag_chat_feed(lt_ag_chat *c, const char *data, size_t n);
int  lt_ag_chat_finish_body(lt_ag_chat *c);           /* 0 ok */
/* the system prompt text */
const char *lt_ag_system_prompt(void);

LT_END_DECLS

#endif
