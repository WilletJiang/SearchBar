#ifndef AH_ASYNC_H
#define AH_ASYNC_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif

/* All methods and delta callbacks run on the engine owner. Each successful start
 * returns a distinct request handle. Borrowed request/callback data stays alive
 * until release. poll advances every outstanding request and waits at most the
 * supplied milliseconds. result sets done=false while pending; once done it
 * transfers malloc-owned response JSON or returns a terminal error. release
 * cancels/frees one handle synchronously and prevents further callbacks. */
typedef struct {
    void *context;
    ah_code (*start)(void *, const char *, ah_delta_sink, void *, ah_cancel_check, void *,
                     void **handle, ah_error *);
    ah_code (*poll)(void *, unsigned timeout_ms, ah_error *);
    ah_code (*result)(void *, void *handle, bool *done, char **response, ah_error *);
    void (*release)(void *, void *handle);
} ah_async_provider;

#ifdef __cplusplus
}
#endif
#endif
