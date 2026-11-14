#include "agents_internal.h"

static const struct {
    const char *name, *description, *schema;
} definitions[] = {
    {"spawn",
     "Delegate to a fresh agent with inherited model and instructions, without any "
     "conversation history. Returns its session ID. Shared tree budgets apply.",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,"
     "\"maxLength\":64},\"message\":{\"type\":\"string\",\"minLength\":1}},"
     "\"required\":[\"name\",\"message\"],\"additionalProperties\":false}"},
    {"send",
     "Queue a message to any agent in this tree by session ID. wake=true starts a new "
     "turn when the recipient is idle; active agents receive it at a model boundary.",
     "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"},"
     "\"message\":{\"type\":\"string\",\"minLength\":1},\"wake\":{\"type\":\"boolean\"}},"
     "\"required\":[\"target\",\"message\"],\"additionalProperties\":false}"},
    {"join",
     "Wait for all selected descendants to complete by default; mode=any waits for one. "
     "Either mode wakes early for failure, stop, user input, approval, unknown effects, "
     "or user steering. Suspends only this agent; results arrive as peer messages.",
     "{\"type\":\"object\",\"properties\":{\"ids\":{\"type\":\"array\",\"minItems\":1,"
     "\"maxItems\":32,\"items\":{\"type\":\"string\"}},\"mode\":{\"type\":\"string\","
     "\"enum\":[\"all\",\"any\"]},\"timeout_ms\":{\"type\":\"integer\","
     "\"minimum\":0,\"maximum\":3600000}},\"required\":[\"ids\"],\"additionalProperties\":false}"},
    {"stop",
     "Interrupt another non-root agent in this tree. Fences late model responses and "
     "preserves external effects that need reconciliation.",
     "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"}},"
     "\"required\":[\"target\"],\"additionalProperties\":false}"},
    {"agents", "List this agent tree, session IDs, states, generations, and shared usage.",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"}};

const char *agents_tool_schema(const char *name) {
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        if (!strcmp(name, definitions[i].name))
            return definitions[i].schema;
    return NULL;
}

bool agents_definitions(yyjson_mut_doc *d, yyjson_mut_val *array) {
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++) {
        yyjson_doc *parsed = yyjson_read(definitions[i].schema, strlen(definitions[i].schema), 0);
        if (!parsed)
            return false;
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, o, "type", "function");
        yyjson_mut_obj_add_str(d, o, "name", definitions[i].name);
        yyjson_mut_obj_add_str(d, o, "description", definitions[i].description);
        yyjson_mut_obj_add_bool(d, o, "strict", false);
        yyjson_mut_obj_add_val(d, o, "parameters",
                               yyjson_val_mut_copy(d, yyjson_doc_get_root(parsed)));
        yyjson_doc_free(parsed);
        if (!yyjson_mut_arr_append(array, o))
            return false;
    }
    return true;
}
