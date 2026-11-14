#ifndef AH_AGENTS_H
#define AH_AGENTS_H
#include "async.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ah_async_provider provider;
    unsigned max_active; /* Simultaneous model requests, including the root. */
    unsigned max_agents; /* Persistent children per root, excluding the root. */
    unsigned max_depth;  /* Root depth is zero. */
} ah_agents_config;

void ah_agents_config_init(ah_agents_config *config);
/* Enable fresh-context delegation on this engine. Call before ah_run. The host
 * owns the provider and all tool contexts through ah_close. ah_run drives the
 * selected root and runnable descendants until quiescent or cancelled. */
ah_code ah_agents_enable(ah_engine *, const ah_agents_config *, ah_error *);
/* Owner-thread operations. IDs are session IDs in the same agent tree. Reusing
 * a message ID with identical contents is idempotent; different contents fail.
 * wake=false queues information, wake=true may start an idle recipient. */
ah_code ah_agent_send(ah_engine *, const char *sender, const char *recipient,
                      const char *message_id, const char *text, bool wake, ah_error *);
ah_code ah_agent_interrupt(ah_engine *, const char *sender, const char *recipient, ah_error *);
ah_code ah_agents_json(ah_engine *, const char *session, char **json, ah_error *);

#ifdef __cplusplus
}
#endif
#endif
