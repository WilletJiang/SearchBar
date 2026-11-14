#include "agent_fixture.h"

static char *yield_reply(agent_fixture *f, agent_request *r) {
    if (strcmp(r->session, f->root)) {
        if (r->ordinal == 1) {
            r->ready = f->tick + 10;
            return af_call("after_yield", "child_effect", "{}");
        }
        return af_text("YIELDED_CHILD_DONE");
    }
    if (r->ordinal == 1)
        return af_call("spawn", "yield_spawn", "{\"name\":\"worker\",\"message\":\"TASK_YIELD\"}");
    af_spawn_id(r->request, "yield_spawn", f->ids[0]);
    if (r->ordinal == 2)
        return af_call("yielding_tool", "owner_effect", "{}");
    yyjson_doc *receipt = af_receipt(r->request, "owner_effect");
    af_result(receipt);
    yyjson_doc_free(receipt);
    return af_text("YIELDED_ROOT_DONE");
}

static ah_code yielding_tool(void *context, const ah_tool_call *call, ah_tool_result *result,
                             ah_error *error) {
    (void)error;
    agent_fixture *f = context;
    assert(pthread_equal(f->owner, pthread_self()) && call->yield);
    assert(!f->phase);
    f->phase = 1;
    agent_request *child = NULL;
    for (unsigned i = 0; i < f->count; i++)
        if (!strcmp(f->requests[i].session, f->ids[0]) && f->requests[i].live)
            child = &f->requests[i];
    assert(child && !child->ready_emitted);
    unsigned completed = f->completed;
    for (unsigned i = 0; i < 20; i++)
        call->yield(call->yield_context);
    assert(child->ready_emitted && !child->transferred && f->completed == completed);
    assert(f->phase == 1); /* A yield pumps transport; it must not reenter tool callbacks. */
    f->phase = 2;
    result->json = strdup("{\"yielded\":true}");
    assert(result->json);
    return AH_OK;
}

static ah_code after_yield(void *context, const ah_tool_call *call, ah_tool_result *result,
                           ah_error *error) {
    (void)call;
    (void)error;
    agent_fixture *f = context;
    assert(pthread_equal(f->owner, pthread_self()) && f->phase == 2);
    f->phase = 3;
    result->json = strdup("{\"effect\":true}");
    assert(result->json);
    return AH_OK;
}

void agent_yield_cases(void) {
    agent_fixture f = {.reply = yield_reply};
    af_open(&f, 2, 8, 4);
    ah_error error = {0};
    ah_tool tool = {.name = "yielding_tool",
                    .description = "Pump transport during an owner effect",
                    .parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}",
                    .effect = AH_EFFECT_WRITE,
                    .execute = yielding_tool,
                    .context = &f};
    af_ok(ah_register_tool(f.engine, &tool, &error), &error);
    tool.name = "after_yield";
    tool.execute = after_yield;
    af_ok(ah_register_tool(f.engine, &tool, &error), &error);
    af_create(&f, "YIELD_ROOT");
    af_run(&f);
    assert(f.phase == 3);
    af_state(&f, f.root, "completed");
    af_state(&f, f.ids[0], "completed");
    af_close(&f);
}
