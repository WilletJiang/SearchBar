# Fresh-context subagents

Version 0.5 adds opt-in delegation through `ah/agents.h`. Every child starts with
its assigned task and the parent's model, host instructions and registered tools.
Conversation items, summaries, previous tool output and historical result access
belong to their original sessions. Each child has a stable session ID and its own
cache routing key. The workspace and host tool contexts are shared.

## Embedding

Create the engine and register tools as usual, then configure delegation before
calling `ah_run`.

```c
#include <ah/agents.h>
#include <ah/responses.h>

ah_agents_config agents;
ah_agents_config_init(&agents);
agents.provider = ah_responses_async_provider(responses);
agents.max_active = 4;
agents.max_agents = 32;
agents.max_depth = 8;
ah_code code = ah_agents_enable(engine, &agents, &error);
if (code == AH_OK)
    code = ah_run(engine, root_session, &error);
```

`max_active` bounds simultaneous model requests across the whole tree, including
the root. `max_agents` bounds persistent children across all turns of a root,
excluding the root. `max_depth` counts edges from the root. The defaults are 4,
32 and 8, respectively. Supported maxima are 32, 1024 and 32. A completed child
can receive follow-up tasks through its existing ID.

`ah_run` drives the root and runnable descendants until quiescent. A parent
parked in `join` leaves capacity available to its descendants. A child waiting
for input or approval can coexist with other working children. The host receives
events with that child's session and operation IDs, then uses the existing
`ah_reply`, `ah_decide` or `ah_resolve` API at an owner boundary. Running the root
again continues its ready children and delivers their results.

The engine retains one SQLite owner. The asynchronous provider executes all
methods and delta callbacks on that owner. The native Responses adapter places
independent easy handles in one libcurl multi connection pool. Each request owns
its parser, routing, cancellation, retry timers and result. The host keeps the
provider and tool contexts alive until the engine has stopped. Token replacement
is available after outstanding request handles have been released.

`ah_async_provider` defines start, poll, result and release. A successful start
returns an independent handle. Poll advances every outstanding handle. Result
transfers a completed JSON response or reports a terminal error. Release cancels
and frees a handle synchronously. An error from poll stops the tree and settles
the active execution state.

Registered effect callbacks execute serially on the owner and can persist their
existing checkpoints. Long callbacks can call `ah_tool_call.yield` to advance
concurrent model transport. Native `exec` and the owner waiting for parallel READ
workers use this hook. It advances I/O while retaining the current effect's
execution boundary. Parallel READ workers receive a null yield hook.

## Model tools

| Tool | Arguments | Behavior |
| --- | --- | --- |
| `spawn` | `name`, `message` | Create a fresh child and return its `id` and name |
| `send` | `target`, `message`, optional `wake` | Send attributed mail within the tree |
| `join` | `ids`, optional `mode`, `timeout_ms` | Park until the selected children settle or attention is needed |
| `stop` | `target` | Interrupt another child, retaining its identity and recorded effects |
| `agents` | empty object | Return IDs, parent relationships, states, generations and usage |

Spawn accepts only its declared arguments. Supply all task-specific facts in
`message`. Child tools use their own session as the sender automatically.

Send defaults to `wake=false`. An idle recipient retains the message until later
work. A recipient working when the message is accepted consumes it at its next
model boundary, including continuation after a final response. `wake=true` can
start an idle recipient's next turn. A stopped recipient requires a newer explicit
wake or host resume. Messages queued before interruption remain available for that
later continuation. External effects requiring reconciliation retain their paused
state until the host resolves them.

Join accepts up to 32 descendant IDs. Its default `mode="all"` waits for all
selected children to complete. A failure, interruption, input request, approval
or unknown outcome returns early with current statuses. `mode="any"` returns when
one selected child settles. The default timeout is 60 seconds, the maximum is one
hour, and zero requests an immediate snapshot. User steering can interrupt a join.
The join receipt contains statuses. Child result messages arrive separately.

Terminal messages carry a bounded preview and an operation ID for `read_result`.
The child and its direct parent can page the complete saved result. Input and
approval messages include the pending operation identity. Private child tool
receipts retain their existing access scope.

## Durability and budgets

Spawn stores the child, parent edge, first message and parent tool receipt in one
transaction keyed by the originating operation ID. Message IDs are unique per
sender. Retrying identical contents acknowledges the existing message. Conflicting
contents fail. Appending received mail to context and marking it consumed share a
transaction. Terminal result publication and its result references are atomic.
The terminal identity includes the child turn, generation, status and attempt
counters so resumed work can publish a later outcome.

Interruption advances a generation and settles operations that had not executed.
Late model results can contribute known usage, while their proposed tool actions
are discarded. Unknown external effects require the existing reconciliation API.
Global cancellation releases request handles and preserves resumable work.

Schema 4 adds the agent graph, mailboxes, wait deadlines and usage ledger. Existing
schema 1–3 databases migrate transactionally. On reopening interrupted work, resume
a stopped root explicitly. Safe interrupted children become runnable before any
parent join evaluates their status. A saved wait retains its original deadline.
Children with uncertain effects retain their reconciliation boundary.

Configured model/tool call limits also bound cumulative attempts across the tree.
Those charges survive follow-up messages, child creation and restart. An optional
root task contract additionally applies its persistent budgets and deadline to
descendants. Child input, approval and unresolved-effect waits postpone task verification
until they settle. `max_active` capacity is allocated by the single owner before each
request is dispatched.

Usage reports are keyed by session, turn and model attempt. `agents` returns
per-agent usage and tree totals for input, cached input, output and reasoning.
Cached and reasoning counts are subsets of their respective totals. Attempts
without a usage report are counted separately in `unreported_attempts`. Cancellation
or process loss can leave such an attempt even when the provider incurred work.

## CLI and host control

```sh
ah run --db agent.db --session ROOT_ID --agents 4 \
  --command-id task-1 --prompt 'Delegate the independent investigations and combine their findings.'
ah agents --db agent.db --session ROOT_ID
ah send --db agent.db --session ROOT_ID --target CHILD_ID --agents 4 \
  --command-id followup-1 --prompt 'Check the remaining case.' --wake
ah run --db agent.db --session ROOT_ID --agents 4
```

`--agent-limit` and `--agent-depth` adjust the creation bounds. RPC exposes
`agents`, `send` and `stop` when started with `--agents N`. The public send,
interrupt, reply and inspection calls use the owner boundary. Cross-thread hosts
can use `ah_cancel` and root `ah_steer` while a tree runs. RPC returns `busy` for
other requests during active execution.

Rebuild embedding clients for the extended 0.5 tool-call struct. The existing
synchronous provider interface remains available for ordinary sessions. See
[validation](validation.md) for concurrency, delivery, recovery and CLI coverage.

The [local concurrency measurements](evaluations/local-performance.md#fresh-context-subagents-2026-11-14) record exact result and request counts alongside timings.
