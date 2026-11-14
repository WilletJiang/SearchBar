#ifndef AH_AGENT_FIXTURE_H
#define AH_AGENT_FIXTURE_H
#include <ah/agents.h>
#include <yyjson.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AF_REQUESTS 128
typedef struct agent_fixture agent_fixture;
typedef struct {
    char session[AH_ID_CAP];
    char *request, *response;
    unsigned ordinal, started, ready, released;
    bool live, transferred, hold, ready_emitted;
    ah_delta_sink delta;
    void *delta_context;
} agent_request;
typedef char *(*agent_reply_fn)(agent_fixture *, agent_request *);
struct agent_fixture {
    ah_engine *engine;
    pthread_t owner;
    char root[AH_ID_CAP], ids[8][AH_ID_CAP];
    const char *database;
    agent_request requests[AF_REQUESTS];
    unsigned count, active, peak, tick, latency, limit, child_started;
    unsigned intents, receipts, spawned, completed;
    unsigned barrier, phase;
    bool interrupted;
    const char *crash_event;
    agent_reply_fn reply;
    void (*on_event)(agent_fixture *, const ah_event *);
};
void af_ok(ah_code, const ah_error *);
void af_open(agent_fixture *, unsigned max_active, unsigned max_agents, unsigned max_depth);
void af_reopen(agent_fixture *);
void af_close(agent_fixture *);
void af_create(agent_fixture *, const char *input);
void af_run(agent_fixture *);
void af_state(agent_fixture *, const char *, const char *);
char *af_text(const char *);
char *af_calls(unsigned, const char *const *, const char *const *, const char *const *);
char *af_call(const char *, const char *, const char *);
char *af_join(const char *call, const char *const *ids, unsigned count, unsigned timeout_ms);
yyjson_doc *af_receipt(const char *request, const char *call);
yyjson_val *af_result(yyjson_doc *);
void af_spawn_id(const char *request, const char *call, char id[AH_ID_CAP]);
void af_terminal_operation(const char *request, const char *child, char id[AH_ID_CAP]);
unsigned af_occurrences(const char *, const char *);
unsigned af_session_calls(const agent_fixture *, const char *);
yyjson_doc *af_agents(agent_fixture *);
void af_remove_database(const char *);
void agent_boundary_cases(void);
void agent_yield_cases(void);
void agent_transaction_cases(void);
void agent_recovery_cases(const char *executable);
int agent_recovery_child(int argc, char **argv);
#endif
