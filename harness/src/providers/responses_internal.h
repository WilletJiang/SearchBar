#ifndef AH_RESPONSES_INTERNAL_H
#define AH_RESPONSES_INTERNAL_H
#include "ah/responses.h"
#include "../internal.h"
#include "stream.h"
#include <curl/curl.h>

struct ah_responses {
    ah_responses_config config;
    char *endpoint, *token;
    CURLM *multi;
    struct responses_request *requests;
    bool loopback, dispatching;
};

typedef struct {
    uint64_t started;
    bool curl_started;
    ah_responses_metrics metrics;
} responses_measurement;

typedef struct responses_request {
    struct responses_request *next;
    ah_responses *provider;
    CURL *easy;
    struct curl_slist *headers;
    const char *json;
    char *response;
    ah_delta_sink delta;
    void *delta_context;
    ah_cancel_check cancelled;
    void *cancel_context;
    ah_stream stream;
    responses_measurement measurement;
    uint64_t deadline, retry_at;
    long status;
    size_t received, header_bytes, limit, request_bytes;
    unsigned attempt, retry_after;
    bool measure, active, done;
    bool content_type, content_type_seen, header_complete;
    ah_error error;
} responses_request;

void responses_stamp(ah_responses_time *value, uint64_t started);
void responses_phase(void *context, ah_stream_phase phase);
void responses_observe(ah_responses *provider, CURL *easy, responses_measurement *measurement);

ah_code responses_check_stop(responses_request *request);
ah_code responses_request_setup(responses_request *request, ah_error *error);
ah_code responses_attempt_start(responses_request *request);
void responses_attempt_finish(responses_request *request, CURLcode result);
void responses_request_free(responses_request *request);

void responses_wipe(char *value);
ah_code responses_complete(void *context, const char *request, ah_delta_sink delta,
                           void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                           char **response, ah_error *error);
#endif
