#ifndef AH_AGENT_SCHEDULER_H
#define AH_AGENT_SCHEDULER_H
#include "agents_internal.h"

typedef struct {
    ah_engine *engine;
    core_session session;
    uint64_t generation, request_generation, deadline, started;
    unsigned request_turn, request_attempt;
    char *request;
    void *handle;
    bool recover, cancelled;
    bool notified;
    unsigned notified_turn, notified_attempt, notified_tools;
    ah_state notified_state;
    uint64_t notified_generation;
} agent_job;

ah_code agents_model_start(agent_job *, ah_error *);
ah_code agents_model_finish(agent_job *, bool *done, ah_error *);
void agents_model_release(agent_job *);
ah_code agents_job_notify(agent_job *, ah_error *);
#endif
