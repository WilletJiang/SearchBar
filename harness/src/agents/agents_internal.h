#ifndef AH_AGENTS_INTERNAL_H
#define AH_AGENTS_INTERNAL_H
#include "../core/core.h"
#include "ah/agents.h"

/* Storage helpers use the engine owner and existing SQLite transactions. */
ah_code agents_schema(ah_engine *, ah_error *);
ah_code agents_root(ah_engine *, const char *session, char root[AH_ID_CAP], ah_error *);
ah_code agents_register_root(ah_engine *, const char *, ah_error *);
ah_code agents_tree_settled(ah_engine *, const char *, bool *, ah_error *);
const char *agents_tool_schema(const char *);
bool agents_definitions(yyjson_mut_doc *, yyjson_mut_val *);
/* Handles/charges a pending coordination operation atomically, including its
 * receipt. join may park it as status='agent_wait'; later calls settle it. */
ah_code agents_execute(ah_engine *, core_session *, core_operation *, ah_error *);
/* Drain acknowledged mail into history at a model boundary, or start a ready
 * turn for a wake message. Return whether anything changed. */
ah_code agents_drain(ah_engine *, core_session *, bool *changed, ah_error *);
ah_code agents_notify(ah_engine *, const core_session *, ah_error *);
ah_code agents_run(ah_engine *, const char *, ah_error *);
/* Owner-serialized durable tree budgets; charge runs inside the caller's intent
 * transaction. Existing optional task budgets additionally apply to the root. */
ah_code agents_allow(ah_engine *, const char *, bool model, unsigned count, unsigned *remaining,
                     ah_error *);
ah_code agents_charge(ah_engine *, const char *, bool model, unsigned count, ah_error *);
/* These primitives run inside an existing transaction; public wrappers guard
 * owner access and commit. The scheduler never calls public wrappers. */
ah_code agents_queue(ah_engine *, const char *, const char *, const char *, const char *, bool,
                     bool internal, ah_error *);
ah_code agents_stop(ah_engine *, const char *, const char *, ah_error *);
ah_code agents_list(ah_engine *, const char *, char **, ah_error *);
ah_code agents_spawn(ah_engine *, const core_session *, const core_operation *, yyjson_val *,
                     char **, ah_error *);
ah_code agents_join(ah_engine *, const core_session *, const core_operation *, yyjson_val *,
                    char **, bool *, ah_error *);
ah_code agents_record_usage(ah_engine *, const core_session *, yyjson_val *, ah_error *);
/* Intent uses model_calls+1 before the in-memory counter advances. A received
 * usage report fills that attempt once; absent reports remain explicitly unknown. */
ah_code agents_usage_intent(ah_engine *, const core_session *, ah_error *);
yyjson_mut_val *agents_usage_value(ah_engine *, yyjson_mut_doc *, const char *, bool, ah_error *);
#endif
