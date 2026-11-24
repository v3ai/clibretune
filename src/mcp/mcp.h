/* mcp.h - read-only MCP (Model Context Protocol) server, G-124.
 *   lt_mcp_handle  JSON-RPC 2.0 dispatcher (initialize, ping, tools/list, tools/call)
 *   lt_mcp_server  opt-in HTTP transport on 127.0.0.1 (POST /mcp, bearer token, Host and
 *                  Origin pinned to loopback), polled once per frame
 *   lt_mcp_stdio   newline-delimited JSON-RPC on stdin/stdout (`clibretune --mcp`)
 * Only the read tools of src/agent are exposed; the propose_* tools are refused.
 * Values are in display units of the loaded definition.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_MCP_H
#define LT_MCP_H

#include "../lt.h"
#include "../agent/agent.h"

LT_BEGIN_DECLS

/* fills env for one request; 0 ok, nonzero = nothing is loaded (tools answer with an error) */
typedef int (*lt_mcp_env_fn)(void *ud, lt_ag_env *env);

/* one JSON-RPC message (or batch) -> malloc'd response text, NULL when none is due
 * (notifications) */
char *lt_mcp_handle(const char *req, size_t len, lt_mcp_env_fn envf, void *ud);

typedef struct lt_mcp_server lt_mcp_server;
/* listen on 127.0.0.1:port (0 = any); NULL on failure (lt_err_get) */
lt_mcp_server *lt_mcp_server_start(int port, const char *token, lt_mcp_env_fn envf, void *ud);
int  lt_mcp_server_port(const lt_mcp_server *s);
void lt_mcp_server_poll(lt_mcp_server *s);       /* never blocks */
void lt_mcp_server_stop(lt_mcp_server *s);

/* serve stdin/stdout until end of input; returns 0 */
int  lt_mcp_stdio(lt_mcp_env_fn envf, void *ud);

LT_END_DECLS

#endif
