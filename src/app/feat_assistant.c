/* feat_assistant.c - the assistant chat tab, its Settings page and the MCP server (G-123 local
 * form, G-124):
 *   Tools -> AI Assistant   chat with an OpenAI-compatible endpoint on plain http://
 *                           (Ollama, LM Studio, llama.cpp or a local TLS proxy); the model reads
 *                           the tune through tools and can only PROPOSE changes, which are listed
 *                           in a review queue and applied after explicit confirmation (a restore
 *                           point is created first)
 *   Settings -> Assistant   endpoint, model, API key (plaintext file, 0600; LT_AI_KEY overrides),
 *                           capability tier, risk acknowledgement, MCP server
 *   MCP                     read-only JSON-RPC tools on 127.0.0.1 (bearer token) or on stdio
 *                           (`clibretune --mcp`)
 * Logic: src/agent (tools, proposals, chat), src/mcp, src/net/http.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../agent/agent.h"
#include "../mcp/mcp.h"

static int cell_label_w(const lt_ui *ui) { return lt_ui_cell_mode(ui) ? 120 : 130; }

typedef struct as_state {
    lt_ag_cfg cfg;
    int loaded;
    lt_ag_chat chat;
    char input[1024];
    char logdir[512];
    lt_mcp_server *mcp;
    int mcp_failed;
    int mcp_port;
    char mcp_err[200];
    char mcp_tok[65];
    int show_token;
    unsigned long seen;
    char notice[200];
} as_state;

static void as_free(lt_app *app, void *p)
{
    as_state *s = (as_state *)p;
    LT_UNUSED(app);
    lt_mcp_server_stop(s->mcp);
    s->mcp = NULL;
    lt_ag_chat_free(&s->chat);
}

static as_state *as_get(lt_app *app)
{
    as_state *s = (as_state *)lt_ext_state(app, "assistant", sizeof(as_state), as_free);
    if (!s->loaded) {
        s->loaded = 1;
        lt_ag_cfg_load(&s->cfg, app->config_dir);
        lt_ag_chat_init(&s->chat);
    }
    return s;
}

static void as_save(lt_app *app, as_state *s)
{
    if (app->config_dir != NULL) { (void)lt_ag_cfg_save(&s->cfg, app->config_dir); }
}

/* the tools' view of the application */
int lt_feat_assistant_env(lt_app *app, lt_ag_env *env)
{
    as_state *s = as_get(app);
    memset(env, 0, sizeof(*env));
    env->ini = app->ini;
    env->src = lt_app_pages(app);
    env->rt = (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) ? app->rt : NULL;
    if (app->has_project) {
        char *d = lt_project_file(&app->project, "datalogs");
        lt_strlcpy(s->logdir, d, sizeof(s->logdir));
        lt_free(d);
        env->log_dir = s->logdir;
    }
    return app->ini != NULL ? 0 : -1;
}

static int mcp_env(void *ud, lt_ag_env *env) { return lt_feat_assistant_env((lt_app *)ud, env); }

int lt_feat_mcp_stdio(lt_app *app) { return lt_mcp_stdio(mcp_env, app); }

/* ---- per-frame service: chat requests and the MCP listener ---- */
static void as_tick(lt_app *app)
{
    as_state *s = as_get(app);
    if (s->chat.busy) {
        lt_ag_env env;
        (void)lt_feat_assistant_env(app, &env);
        (void)lt_ag_chat_step(&s->chat, &s->cfg, &env);
    }
    if (s->cfg.mcp_on && s->mcp == NULL && !s->mcp_failed) {
        if (lt_ag_token(app->config_dir != NULL ? app->config_dir : ".", 0, s->mcp_tok) == 0) {
            s->mcp = lt_mcp_server_start(s->cfg.mcp_port, s->mcp_tok, mcp_env, app);
        }
        if (s->mcp == NULL) {
            s->mcp_failed = 1;
            lt_strlcpy(s->mcp_err, lt_err_get(), sizeof(s->mcp_err));
        } else {
            s->mcp_port = s->cfg.mcp_port;
        }
    }
    if ((!s->cfg.mcp_on || (s->mcp != NULL && s->mcp_port != s->cfg.mcp_port)) && s->mcp != NULL) {
        lt_mcp_server_stop(s->mcp);
        s->mcp = NULL;
    }
    if (s->mcp != NULL) { lt_mcp_server_poll(s->mcp); }
}

/* ---- chat tab ---- */
static void send_message(lt_app *app, as_state *s)
{
    lt_app_tab *tab = lt_app_tab_active(app);
    s->chat.context[0] = '\0';
    if (app->signature[0] != '\0') {
        lt_snprintf(s->chat.context, sizeof(s->chat.context), "ECU signature: %s.", app->signature);
    }
    if (tab != NULL && strcmp(tab->kind, "table") == 0) {
        size_t n = strlen(s->chat.context);
        lt_snprintf(s->chat.context + n, sizeof(s->chat.context) - n, " The user has table '%s' open.",
                    tab->target);
    }
    if (lt_ag_chat_send(&s->chat, &s->cfg, s->input) == 0) { s->input[0] = '\0'; }
}

static void stage_accepted(lt_app *app, as_state *s)
{
    lt_pagesrc ps = lt_app_pages(app);
    char *rp = NULL;
    int n;
    if (app->ini == NULL || lt_ag_props_accepted(&s->chat.props) == 0) { return; }
    if (app->has_project) { (void)lt_restore_create(&app->project, &rp); }
    n = lt_ag_props_apply(&s->chat.props, app->ini, &ps);
    if (n < 0) {
        lt_app_toast(app, 3, "Could not apply the proposal: %s", lt_err_get());
    } else {
        lt_app_edited(app);
        lt_app_toast(app, 1, "Staged %d change%s%s (nothing was burned)", n, n == 1 ? "" : "s",
                     rp != NULL ? "; a restore point was created first" : "");
    }
    lt_free(rp);
}

static void draw_message(lt_ui *ui, const lt_theme *th, const lt_json *m)
{
    const char *role = lt_json_get_str(m, "role", "");
    const char *text = lt_json_get_str(m, "content", "");
    const lt_json *tcs = lt_json_get(m, "tool_calls");
    size_t i;
    char line[320];
    int w1 = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (strcmp(role, "user") == 0) {
        lt_ui_label_ex(ui, "You", th->accent, LT_UI_ALIGN_LEFT);
        lt_ui_label_wrap(ui, text, th->text);
    } else if (strcmp(role, "assistant") == 0) {
        if (text[0] != '\0') {
            lt_ui_label_ex(ui, "Assistant", th->primary_hover, LT_UI_ALIGN_LEFT);
            lt_ui_label_wrap(ui, text, th->text);
        }
        for (i = 0; tcs != NULL && i < tcs->n; i++) {
            const lt_json *f = lt_json_get(lt_json_at(tcs, i), "function");
            lt_snprintf(line, sizeof(line), "  > %s(%s)", lt_json_get_str(f, "name", "?"),
                        lt_json_get_str(f, "arguments", ""));
            lt_ui_label_ex(ui, line, th->muted, LT_UI_ALIGN_LEFT);
        }
    }
    lt_ui_layout_space(ui, 4);
}

static void queue_draw(lt_app *app, as_state *s, const lt_theme *th)
{
    lt_ui *ui = app->ui;
    lt_ag_props *p = &s->chat.props;
    int w1 = -1, wr[2], i;
    char line[300];
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(line, sizeof(line), "Review queue: %lu proposal%s (%lu accepted)",
                (unsigned long)p->n, p->n == 1 ? "" : "s", (unsigned long)lt_ag_props_accepted(p));
    lt_ui_label_ex(ui, line, th->warning, LT_UI_ALIGN_LEFT);
    lt_ui_layout_row(ui, 1, &w1, lt_ui_cell_mode(ui) ? 5 * lt_ui_row_height(ui) : 130);
    if (lt_ui_scroll_begin(ui, "as_queue")) {
        for (i = 0; i < (int)p->n; i++) {
            lt_ag_prop *q = &p->v[i];
            int acc = q->state == LT_AG_ACCEPTED;
            char desc[200];
            lt_ui_push_id_int(ui, i);
            wr[0] = 28; wr[1] = -1;
            lt_ui_layout_row(ui, 2, wr, 0);
            if (q->applied) { lt_ui_label_ex(ui, "ok", th->success, LT_UI_ALIGN_LEFT); }
            else if (!q->valid) { lt_ui_label_ex(ui, "x", th->error, LT_UI_ALIGN_LEFT); }
            else if (lt_ui_checkbox(ui, "", &acc)) { q->state = acc ? LT_AG_ACCEPTED : LT_AG_REJECTED; }
            lt_ag_prop_describe(q, desc, sizeof(desc));
            lt_snprintf(line, sizeof(line), "%s%s", q->dangerous ? "[DANGEROUS] " : "", desc);
            lt_ui_label_ex(ui, line, q->valid ? th->text : th->error, LT_UI_ALIGN_LEFT);
            if (q->note[0] != '\0' || q->reason[0] != '\0') {
                lt_ui_layout_row(ui, 2, wr, 0);
                lt_ui_label(ui, "");
                lt_snprintf(line, sizeof(line), "%s%s%s", q->reason, (q->reason[0] && q->note[0]) ? " | " : "",
                            q->note);
                lt_ui_label_ex(ui, line, th->muted, LT_UI_ALIGN_LEFT);
            }
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
    {
        int wb[4], k, any_pending = 0;
        wb[0] = 0; wb[1] = 0; wb[2] = 0; wb[3] = -1;
        for (k = 0; k < (int)p->n; k++) { if (p->v[k].valid && !p->v[k].applied) { any_pending = 1; } }
        lt_ui_layout_row(ui, 4, wb, 0);
        if (lt_ui_button_ex(ui, "Accept all valid", any_pending ? 0 : LT_UI_BTN_DISABLED)) {
            for (k = 0; k < (int)p->n; k++) { if (p->v[k].valid && !p->v[k].applied) { p->v[k].state = LT_AG_ACCEPTED; } }
        }
        if (lt_ui_button_ex(ui, "Reject all", any_pending ? 0 : LT_UI_BTN_DISABLED)) {
            for (k = 0; k < (int)p->n; k++) { if (!p->v[k].applied) { p->v[k].state = LT_AG_REJECTED; } }
        }
        if (lt_ui_button_ex(ui, "Stage accepted changes",
                            lt_ag_props_accepted(p) > 0 ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED)) {
            stage_accepted(app, s);
        }
        lt_ui_label(ui, "");
    }
}

static void assistant_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    as_state *s = as_get(app);
    int w1 = -1, wi[3], wh[3], i, fl, usable, reserve;
    char line[300];
    int cell = lt_ui_cell_mode(ui);
    LT_UNUSED(tab);
    LT_UNUSED(r);
    usable = s->cfg.enabled && s->cfg.ack;

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "AI Assistant");
    wh[0] = 0; wh[1] = 0; wh[2] = -1;
    lt_ui_layout_row(ui, 3, wh, 0);
    if (lt_ui_button_ex(ui, "New chat", s->chat.busy ? LT_UI_BTN_DISABLED : LT_UI_BTN_FLAT)) {
        lt_ag_chat_reset(&s->chat);
    }
    if (lt_ui_button_ex(ui, "Settings", LT_UI_BTN_FLAT)) { (void)lt_app_settings_dialog(app); }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_snprintf(line, sizeof(line), "%s | model %s | tier %s | tokens %ld", s->cfg.base_url, s->cfg.model,
                lt_ag_tier_name(s->cfg.tier), s->chat.n_tok);
    lt_ui_label_ex(ui, line, th->muted, LT_UI_ALIGN_LEFT);
    if (!usable) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "The assistant is off. Open Settings > Assistant, acknowledge the risk "
                         "notice and enable it. Changes are only ever proposed; nothing is applied "
                         "or burned without your confirmation.", th->warning);
    }

    reserve = (s->chat.props.n > 0 ? (cell ? 9 * lt_ui_row_height(ui) : 215) : 0) + (cell ? 3 * lt_ui_row_height(ui) : 70);
    lt_ui_layout_row(ui, 1, &w1, -reserve);
    if (lt_ui_scroll_begin(ui, "as_chat")) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->chat.msgs->n == 0 && !s->chat.busy) {
            lt_ui_label_wrap(ui, "Ask about your tune, e.g. \"Review my VE table\", \"What is the engine doing "
                             "right now?\" or \"Summarize my last datalog\".", th->muted);
        }
        for (i = 0; i < (int)s->chat.msgs->n; i++) { draw_message(ui, th, s->chat.msgs->items[i]); }
        if (s->chat.busy) {
            if (s->chat.reply.len > 0) {
                lt_ui_label_ex(ui, "Assistant", th->primary_hover, LT_UI_ALIGN_LEFT);
                lt_ui_label_wrap(ui, s->chat.reply.data, th->text);
            }
            lt_ui_label_ex(ui, s->chat.status, th->muted, LT_UI_ALIGN_LEFT);
        }
        if (s->chat.error[0] != '\0') { lt_ui_label_wrap(ui, s->chat.error, th->error); }
        if (s->chat.changes != s->seen) { lt_ui_scroll_set(ui, 0, 1 << 20); s->seen = s->chat.changes; }
        lt_ui_scroll_end(ui);
    }
    if (s->chat.props.n > 0) { queue_draw(app, s, th); }

    wi[0] = -1; wi[1] = cell ? 10 * 8 : 96; wi[2] = 0;
    lt_ui_layout_row(ui, 2, wi, 0);
    fl = lt_ui_text(ui, "as_in", s->input, sizeof(s->input), 0);
    if (s->chat.busy) {
        if (lt_ui_button_ex(ui, "Stop", LT_UI_BTN_DANGER)) { lt_ag_chat_cancel(&s->chat); }
    } else {
        int can = usable && s->input[0] != '\0';
        if ((lt_ui_button_ex(ui, "Send", can ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && can) ||
            ((fl & LT_UI_SUBMIT) && can)) {
            lt_u32 id = lt_ui_last_id(ui);
            send_message(app, s);
            LT_UNUSED(id);
        }
    }
}

static int open_assistant(lt_app *app, const char *t)
{
    LT_UNUSED(t);
    (void)as_get(app);
    return lt_app_tab_open(app, "assistant", "assistant", "AI Assistant", 1) != NULL ? 0 : -1;
}

static void act_assistant(lt_app *app) { (void)open_assistant(app, "assistant"); }

/* ---- settings page ---- */
static void settings_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    as_state *s = as_get(app);
    lt_ag_cfg *c = &s->cfg;
    static const char *const tiers[] = { "Read: inspect and diagnose only",
                                         "Tune: read + propose table edits",
                                         "Config: tune + propose constant changes" };
    int w1 = -1, w2[2], ch = 0, sel, reset_ack = 0;
    char line[300];
    w2[0] = cell_label_w(ui); w2[1] = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "AI Assistant (at your own risk)");
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "A model you run yourself reads your tune through tools and can propose "
                     "changes that affect engine behavior. Proposals are validated, limited to the "
                     "AutoTune authority limits and applied only after you confirm; a restore point is "
                     "created first and nothing is burned. Only plain http:// endpoints work (no TLS): "
                     "Ollama, LM Studio, llama.cpp or a local proxy.", th->muted);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "I understand the risks", &c->ack)) { ch = 1; }
    if (lt_ui_checkbox(ui, "Enable AI Assistant", &c->enabled)) { ch = 1; }
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Base URL");
    if (lt_ui_text(ui, "as_url", c->base_url, sizeof(c->base_url), 0) & LT_UI_CHANGED) { ch = 1; reset_ack = 1; }
    lt_ui_label(ui, "Model");
    if (lt_ui_text(ui, "as_model", c->model, sizeof(c->model), 0) & LT_UI_CHANGED) { ch = 1; }
    lt_ui_label(ui, "API key");
    if (lt_ui_text(ui, "as_key", c->api_key, sizeof(c->api_key), LT_UI_TEXT_PASSWORD) & LT_UI_CHANGED) {
        ch = 1; reset_ack = 1;
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "The key is optional for local endpoints. It is stored in plaintext in "
                     "assistant.conf (file mode 0600) in the settings folder; set the environment "
                     "variable LT_AI_KEY instead to keep it out of the file.", th->muted);
    {
        int wp[3];
        wp[0] = 0; wp[1] = 0; wp[2] = -1;
        lt_ui_layout_row(ui, 3, wp, 0);
        if (lt_ui_button(ui, "Ollama preset")) {
            lt_strlcpy(c->base_url, "http://127.0.0.1:11434/v1", sizeof(c->base_url)); ch = 1; reset_ack = 1;
        }
        if (lt_ui_button(ui, "LM Studio preset")) {
            lt_strlcpy(c->base_url, "http://127.0.0.1:1234/v1", sizeof(c->base_url)); ch = 1; reset_ack = 1;
        }
        lt_ui_label(ui, "");
    }
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Capability tier");
    sel = c->tier;
    if (lt_ui_combo(ui, "as_tier", &sel, tiers, 3)) { c->tier = sel; ch = 1; }
    if (strncmp(c->base_url, "https://", 8) == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "https:// is not supported. Use a local endpoint or a proxy that terminates TLS.",
                         th->error);
    }

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_separator(ui);
    lt_ui_heading(ui, "MCP server (local, read-only)");
    if (lt_ui_checkbox(ui, "Expose read-only tools over MCP on 127.0.0.1", &c->mcp_on)) {
        ch = 1;
        s->mcp_failed = 0;
    }
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_label(ui, "Port");
    {
        double d = (double)c->mcp_port;
        if (lt_ui_number(ui, "as_port", &d, 1024.0, 65535.0, 1.0, 0)) {
            c->mcp_port = (int)d; ch = 1; s->mcp_failed = 0;
        }
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (s->mcp != NULL) {
        lt_snprintf(line, sizeof(line), "Running on 127.0.0.1:%d", lt_mcp_server_port(s->mcp));
        lt_ui_label_ex(ui, line, th->success, LT_UI_ALIGN_LEFT);
    } else if (s->mcp_failed) {
        lt_ui_label_wrap(ui, s->mcp_err, th->error);
    } else {
        lt_ui_label_ex(ui, "Stopped", th->muted, LT_UI_ALIGN_LEFT);
    }
    if (lt_ag_token(app->config_dir != NULL ? app->config_dir : ".", 0, s->mcp_tok) == 0) {
        int wt[3];
        wt[0] = cell_label_w(ui); wt[1] = -1; wt[2] = 0;
        lt_ui_layout_row(ui, 3, wt, 0);
        lt_ui_label(ui, "Access token");
        if (s->show_token) { lt_ui_label_ex(ui, s->mcp_tok, th->text2, LT_UI_ALIGN_LEFT); }
        else { lt_ui_label_ex(ui, "(hidden)", th->muted, LT_UI_ALIGN_LEFT); }
        if (lt_ui_button(ui, s->show_token ? "Hide" : "Show")) { s->show_token = !s->show_token; }
        wt[0] = 0; wt[1] = -1;
        lt_ui_layout_row(ui, 2, wt, 0);
        if (lt_ui_button(ui, "Regenerate")) {
            (void)lt_ag_token(app->config_dir != NULL ? app->config_dir : ".", 1, s->mcp_tok);
            s->mcp_failed = 0;
            if (s->mcp != NULL) { lt_mcp_server_stop(s->mcp); s->mcp = NULL; }
        }
        lt_ui_label(ui, "");
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, "HTTP: POST http://127.0.0.1:PORT/mcp with the header \"Authorization: Bearer "
                     "TOKEN\". Without HTTP, run `clibretune --mcp --project DIR` and speak JSON-RPC over "
                     "stdin/stdout. Only the read tools are offered.", th->muted);
    if (reset_ack && c->ack) { c->ack = 0; }
    if (ch) { as_save(app, s); }
}

void lt_feat_assistant_register(lt_app_ext *reg)
{
    lt_app_tab_kind tk;
    lt_ext_add_target(reg, "assistant", open_assistant);
    tk.kind = "assistant"; tk.draw = assistant_draw; tk.close = NULL;
    lt_ext_add_tab_kind(reg, &tk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&AI Assistant", NULL, act_assistant, NULL);
    lt_ext_add_settings_page(reg, "Assistant", settings_page);
    lt_ext_add_tick(reg, as_tick);
}
