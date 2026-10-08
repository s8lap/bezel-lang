// bezel_runtime_events.cpp — EventSource / event handler lists (spec §24).
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// Events (language spec §24). An EventSource holds an ordered handler list.
// Each handler is a Bezel closure value (a { fn_ptr, env_ptr } struct pointer).
// Raise is lowered in codegen; the runtime only stores/produces the list.
// ---------------------------------------------------------------------------

struct bz_event_handler_t {
    void*   closure;  // closure { fn_ptr, env_ptr } struct pointer
    int64_t token;    // unique, monotonically increasing (>0)
    bool    active;   // false once removed (tombstone)
};

struct bz_event_source_t {
    bz_event_handler_t* handlers;
    int64_t             count;     // number of slots in use (incl. tombstones)
    int64_t             capacity;
    int64_t             next_token;
};

// #1413 (stdlib §6.1): CancellationToken.Register installs a `() -> ()` callback
// on the token's cancel-state cell (bz_cancel_state_t, defined in the cancellation
// section far below).  The Subscription that Register returns REUSES the event
// machinery's bz_subscription_t + bz_subscription_dispose (and the live-set
// idempotency), routing its unregistration to the cancel-callback list instead of
// an event source.  Forward-declare the state type and the one static helper that
// bz_subscription_dispose needs; both live in the cancellation section below.
struct bz_cancel_state_t;


extern "C" void* bz_event_source_new(void) {
    auto* s = (bz_event_source_t*)calloc(1, sizeof(bz_event_source_t));
    s->next_token = 1;  // tokens are >0 so 0 can mean "none"
    return s;
}

// §24 / §22.13 (#477 B6): a STATIC event's backing EventSource is a static
// owned field with PROGRAM-EXIT disposal. Track the static sources in a global
// list and free them (handlers + struct) via an atexit handler installed once,
// so the program-exit-disposal obligation is met across every exit path
// (normal return, early return, or an unhandled-exception interceptor that
// still drains atexit). Allocated once per static event at program start.
static std::vector<bz_event_source_t*>* g_static_event_sources = nullptr;

static void bz_free_static_event_sources(void) {
    if (!g_static_event_sources) return;
    for (bz_event_source_t* s : *g_static_event_sources) {
        if (!s) continue;
        bz_event_source_free(s);
    }
    delete g_static_event_sources;
    g_static_event_sources = nullptr;
}

extern "C" void* bz_static_event_source_new(void) {
    auto* s = (bz_event_source_t*)calloc(1, sizeof(bz_event_source_t));
    s->next_token = 1;
    if (!g_static_event_sources) {
        g_static_event_sources = new std::vector<bz_event_source_t*>();
        atexit(bz_free_static_event_sources);
    }
    g_static_event_sources->push_back(s);
    return s;
}

extern "C" int64_t bz_event_add(void* srcp, void* closure) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return 0;
    if (s->count >= s->capacity) {
        int64_t nc = s->capacity < 4 ? 4 : s->capacity * 2;
        // Capture the realloc result: on failure the original block is still
        // valid and must not be overwritten with NULL (would leak it and then
        // NULL-deref on the writes below).
        void* tmp = realloc(s->handlers,
                            (size_t)nc * sizeof(bz_event_handler_t));
        if (!tmp) return 0;
        s->handlers = (bz_event_handler_t*)tmp;
        s->capacity = nc;
    }
    int64_t tok = s->next_token++;
    s->handlers[s->count].closure = closure;
    s->handlers[s->count].token = tok;
    s->handlers[s->count].active = true;
    s->count++;
    return tok;
}

extern "C" int64_t bz_event_count(void* srcp) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return 0;
    int64_t n = 0;
    for (int64_t i = 0; i < s->count; ++i)
        if (s->handlers[i].active) ++n;
    return n;
}

extern "C" void* bz_event_handler_at(void* srcp, int64_t idx) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return nullptr;
    int64_t n = 0;
    for (int64_t i = 0; i < s->count; ++i) {
        if (!s->handlers[i].active) continue;
        if (n == idx) return s->handlers[i].closure;
        ++n;
    }
    return nullptr;
}

// §27.7.1 (v0.180, #1544): two function values are the SAME under the spec's
// equality rule, which is KIND-DISPATCHED because the runtime cannot otherwise
// distinguish a lambda from a method/property reference, nor a class-receiver
// reference from a record-receiver reference (all are {fn, env, rc, envdtor,
// kind} cells). Codegen tags each cell with a FnValKind (off
// BZ_CLOSURE_KIND_OFFSET = 32; see bezel_runtime_internal.hpp):
//
//   * BZ_FVK_REF_IDENTITY (0, default): a lambda or a value-shaped-receiver
//     (record/enum/primitive) method/property reference. Such a value equals
//     ONLY itself — §27.7.1 first bullet (lambdas) and third bullet (record
//     receivers have no identity; two separate evaluations are distinct copies).
//     So after the identical-wrapper fast path (a == b), two DISTINCT cells of
//     this kind are NEVER equal.
//   * BZ_FVK_FN_PLUS_RECV (1): a class/interface-receiver method/property
//     reference. Equal iff same fn (thunk) AND same receiver pointer — §27.7.1
//     second bullet ("repeated evaluations of obj.Method on the same class-typed
//     obj are guaranteed equal"). Each reference formation mints a FRESH env
//     block, so the env pointers differ; the receiver is the env's first word.
//   * BZ_FVK_FN_ONLY (2): a static (or unbound-extension) method/property
//     reference. Equal iff same fn — §27.7.1 second bullet ("a static ...
//     reference to M equals every other reference to M"). The env is null.
//
// Distinct kinds are never equal (§27.7.1 fourth bullet: "a lambda never
// equals a method reference"). The identical-wrapper fast path (a == b) serves
// the store-once pattern for ALL kinds (a stored value compared/removed via the
// same binding) — load-bearing for §24 `-=` matching of a stored lambda or a
// record-receiver named method (which, lacking identity, can only match itself).
//
// Layout per bz_closure_release: fn at off 0, env at off 8. The env for a
// method/property thunk holds the receiver as its first word and is always
// >= 8 bytes, so reading it is in bounds; the read is reached only when both
// the kinds and the fn pointers already match.
static bool bz_closure_same(void* a, void* b) {
    if (a == b) return true;        // identical wrapper (or both null)
    if (!a || !b) return false;
    int64_t a_kind, b_kind;
    memcpy(&a_kind, (uint8_t*)a + BZ_CLOSURE_KIND_OFFSET, sizeof(int64_t));
    memcpy(&b_kind, (uint8_t*)b + BZ_CLOSURE_KIND_OFFSET, sizeof(int64_t));
    if (a_kind != b_kind) return false;   // §27.7.1: lambda never equals a ref
    void* a_fn; void* b_fn;
    memcpy(&a_fn, a, sizeof(void*));
    memcpy(&b_fn, b, sizeof(void*));
    if (a_fn != b_fn) return false;       // different method/thunk → never equal
    // Same kind + same fn. Apply the per-kind receiver rule.
    if (a_kind == BZ_FVK_FN_ONLY) return true;        // static: fn match suffices
    if (a_kind == BZ_FVK_REF_IDENTITY) return false;   // lambda/value-receiver: only a==b (already false)
    // BZ_FVK_FN_PLUS_RECV: class/interface receiver — compare receiver ptrs.
    void* a_env; void* b_env;
    memcpy(&a_env, (uint8_t*)a + 8, sizeof(void*));  // env ptr at offset 8
    memcpy(&b_env, (uint8_t*)b + 8, sizeof(void*));
    if (a_env == b_env) return true;        // same env block: match
    if (!a_env || !b_env) return false;     // one has a receiver, one not
    void* a_recv; void* b_recv;
    memcpy(&a_recv, a_env, sizeof(void*));   // receiver = first word of env
    memcpy(&b_recv, b_env, sizeof(void*));
    return a_recv == b_recv;
}

// §27.7.1 (v0.180) / declared in bezel_runtime.h: the general `=`/`<>`
// function-value equality operator delegates to the same {fn, receiver}
// match `-=` already uses (bz_closure_same above) instead of a raw closure-
// wrapper pointer compare — see the header comment for the rationale.
extern "C" int32_t bz_closure_value_equals(void* a, void* b) {
    return bz_closure_same(a, b) ? 1 : 0;
}

extern "C" void bz_event_remove_closure(void* srcp, void* closure) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return;
    // §24: -= removes the FIRST matching handler. A handler matches by the
    // closure wrapper's { fn, env } VALUE, not by wrapper-pointer identity, so
    // a method-group `-=` removes the corresponding `+=`. // per §24 (#1170)
    for (int64_t i = 0; i < s->count; ++i) {
        if (s->handlers[i].active &&
            bz_closure_same(s->handlers[i].closure, closure)) {
            s->handlers[i].active = false;
            return;
        }
    }
}

extern "C" void bz_event_remove_token(void* srcp, int64_t token) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s || token == 0) return;
    for (int64_t i = 0; i < s->count; ++i) {
        if (s->handlers[i].token == token) {
            s->handlers[i].active = false;  // idempotent: re-clearing is harmless
            return;
        }
    }
}

// Set of live (subscribed, not-yet-disposed) bz_subscription_t* nodes. A node
// is inserted by bz_event_subscribe and erased+freed by bz_subscription_dispose.
// Membership is the authority for "is this handle still alive": Dispose must be
// idempotent (§2.11), but the fix below frees the node on dispose, so a second
// Dispose with the same (now-freed) pointer must NOT dereference it. Checking
// the set before any field access keeps the second call a safe no-op with no
// use-after-free. (Codegen relies on Subscription.Dispose being idempotent —
// e.g. an explicit `.Dispose()` plus a scope-exit/`using var` disposal both
// hand the same pointer here.) Guarded by a mutex because subscriptions, like
// the rest of the events runtime, may be touched from multiple threads.
std::mutex&                     bz_sub_live_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_set<void*>&      bz_sub_live_set() {
    static std::unordered_set<void*> s;
    return s;
}

extern "C" void* bz_event_subscribe(void* srcp, void* closure) {
    auto* s = (bz_event_source_t*)srcp;
    int64_t tok = bz_event_add(s, closure);
    auto* sub = (bz_subscription_t*)calloc(1, sizeof(bz_subscription_t));
    sub->src = s;
    sub->token = tok;
    sub->disposed = false;
    {
        std::lock_guard<std::mutex> lk(bz_sub_live_mutex());
        bz_sub_live_set().insert(sub);
    }
    return sub;
}

extern "C" void bz_subscription_dispose(void* subp) {
    if (!subp) return;
    {
        // §2.11: Dispose is idempotent. Only a live (registered) node is acted
        // on; a second call finds the pointer absent and returns without ever
        // dereferencing the freed node (no double-free, no use-after-free).
        std::lock_guard<std::mutex> lk(bz_sub_live_mutex());
        if (bz_sub_live_set().erase(subp) == 0) return;
    }
    auto* sub = (bz_subscription_t*)subp;
    sub->disposed = true;
    if (sub->cancel_state) {
        // #1413 (stdlib §6.1): a CancellationToken.Register subscription.  Remove
        // the callback from the cancel-state's callback list (bz_ctoken_unregister
        // releases the transferred +1 IF the callback is still installed — if a
        // concurrent Cancel already snapshot-and-drained it, unregister finds
        // nothing and Cancel owns the release, so the +1 is dropped exactly once).
        // Then drop the subscription's retained reference on the state cell.  This
        // is the callback-list analogue of the event path below, and it is the ONLY
        // dispose entry point codegen emits for a Subscription (§2.11 idempotent).
        bz_cancel_state_t* cs = sub->cancel_state;
        sub->cancel_state = nullptr;
        bz_ctoken_unregister(cs, sub->token);
        bz_cancel_state_release(cs);
        free(sub);
        return;
    }
    if (sub->src) {
        // §24 / §27.5 / #841: the handler closure is a transferred +1 — codegen
        // (lowerEventHandlerClosure → bz_event_subscribe) hands the event source
        // the SOLE reference with no separate retain.  The source owns exactly one
        // reference and must release it exactly once on unsubscribe.  Find the
        // handler by token, release its closure (bz_closure_release is null-safe so
        // non-capturing lambdas with a null env-pointer are handled correctly), and
        // null the slot before deactivating — this prevents any stale pointer read
        // should bz_event_handler_at or the raise loop ever see the inactive entry.
        // The idempotency guard (bz_sub_live_set erase == 0) above ensures this
        // path runs AT MOST ONCE per subscription, so no double-release is possible.
        bz_event_source_t* s = sub->src;
        for (int64_t i = 0; i < s->count; ++i) {
            if (s->handlers[i].token == sub->token) {
                void* cl = s->handlers[i].closure;
                s->handlers[i].closure = nullptr;  // null before release (defensive)
                s->handlers[i].active  = false;    // deactivate (tombstone)
                bz_closure_release(cl);             // release the transferred +1
                break;
            }
        }
        // bz_event_remove_token is a no-op now (already deactivated above), but
        // calling it here would be redundant — skip it.
    }
    sub->src = nullptr;  // null the back-pointer before freeing
    free(sub);           // fixes #372: the node was previously never freed
}

extern "C" void bz_event_source_free(void* srcp) {
    auto* s = (bz_event_source_t*)srcp;
    if (!s) return;
    // fix #4976 (stdlib §6.5 v0.315): per §24 / §27.5 / #841, the source OWNS
    // exactly one reference on every registered handler closure — codegen's
    // bz_event_add / bz_event_subscribe hand it a transferred +1 the source
    // must release exactly once. Before this fix bz_event_source_free only
    // freed the handler array and the struct, leaking every still-active
    // handler closure. Iterate and release each non-null closure: bz_event_add
    // stores a non-null pointer, `bz_subscription_dispose` nulls the closure
    // before releasing its +1 (so a subscription-disposed handler is already
    // accounted for), and `bz_event_remove_closure` (Remove / `-=`) only
    // deactivates without nulling — its matched handler must be released
    // here (or earlier, in the same statement, by the #4970 caller-side fix).
    for (int64_t i = 0; i < s->count; ++i) {
        void* cl = s->handlers[i].closure;
        if (cl) {
            s->handlers[i].closure = nullptr;
            bz_closure_release(cl);
        }
    }
    free(s->handlers);
    free(s);
}

// fix #1832 / stdlib §6.5 (v0.187): the cross-domain publication-point pair
// SharedEventSource<T> / ISharedEvent<T>. The runtime representation is the
// same bz_event_source_t* the instance pair uses — the synchronized
// quiescence / lock discipline around the subscriber list (the contract that
// subscribe/raise/Subscription.Dispose are safe to reach from every domain) is
// recorded for a follow-up slice; this first pair of entry points admits the
// type names by mirroring the instance surface, leaving room for a future
// mutex slot in the struct without changing the codegen contract.
//
// `bz_shared_event_source_new` deliberately routes through the same allocation
// path bz_event_source_new uses (calloc + token=1), so the cross-domain path
// cannot drift in handler-list initialization; the lock discipline will plug
// in by mutating the returned struct's lock slot, not by changing how the
// struct is allocated. Likewise `bz_shared_event_source_free` mirrors
// bz_event_source_free: null-safe, severs the subscriber list, frees the
// struct — the lock acquired at free time is a release-acquire detail that
// does not change the shape.
extern "C" void* bz_shared_event_source_new(void) {
    // §6.5 (v0.187) / fix #1832: same allocation as bz_event_source_new —
    // future synchronization will live in a struct slot, not in the
    // allocation path.
    return bz_event_source_new();
}

extern "C" void bz_shared_event_source_free(void* srcp) {
    // §6.5 (v0.187) / fix #1832: null-safe, same reclaim shape as
    // bz_event_source_free. Future synchronized quiescence plugs in here.
    bz_event_source_free(srcp);
}

// per §24 / §30.13 / #1377: async-event RaiseAsync via continuation chaining.
//
// Problem with the prior implementation (bz_task_await_sync loop inside the
// raiser's continuation): bz_task_await_sync blocks the OS thread, which is
// the domain's drain worker.  When a handler genuinely suspends (e.g. awaits
// Task.Delay) the domain can never process the handler's resume step →
// deadlock.  §30.13: "a continuation blocks its whole domain."
//
// Fix: replace the blocking loop with a continuation chain via bz_task_then so
// the domain drain thread is never blocked:
//   1. Allocate an outer Task (the RaiseAsync return value).
//   2. Invoke handler[0] via the SHIM (a per-call-site codegen function that
//      knows the exact LLVM payload type).
//   3. Register bz_raise_chain_step via bz_task_then on handler[0]'s task.
//      - already-done  → step is enqueued to the domain (no blocking).
//      - pending       → step fires when the handler's timer/await completes.
//   4. bz_raise_chain_step:
//      - if faulted → fault the outer task (first-error-wins §24); done.
//      - else       → free completed task, invoke handler[i+1]; repeat.
//      - if all done → complete the outer task.
//
// Payload: codegen generates a per-call-site SHIM function
//   (ptr closure, ptr payload_buf) → ptr (Task*)
// that loads the payload from payload_buf with the correct LLVM type and
// invokes the handler's fn(env, payload?).  The runtime memcpys the caller's
// alloca into a heap buffer (ctx->payload_buf) so subsequent chain steps can
// read the payload after the originating continuation has returned.
//
// §24: the subscriber list is unsynchronized — single-domain run-to-
// completion serializes all continuation steps within a domain.  count is
// snapshotted at raise time; bz_event_handler_at is re-read live at each step
// so handlers deactivated mid-raise are skipped (matching sync Raise, #908).

typedef void* (*bz_handler_shim_t)(void* closure, void* payload_buf);

struct BzRaiseChainCtx {
    bz_task_t*          outer;        // outer Task returned by RaiseAsync
    bz_event_source_t*  src;          // event source (no retain; outlives the frame)
    int64_t             count;        // active-handler count snapshotted at raise time
    int64_t             index;        // NEXT handler index to invoke (0-based, active)
    void*               payload_buf;  // heap copy of payload (null = no payload)
    bz_handler_shim_t   shim;         // codegen shim: (closure, payload_buf) → Task*
};

static void bz_raise_chain_step(void* completed_task, void* state);

// Invoke the next active handler in the chain, or complete outer on exhaustion.
static void bz_raise_invoke_next(BzRaiseChainCtx* ctx) {
    while (ctx->index < ctx->count) {
        void* closure = bz_event_handler_at(ctx->src, ctx->index);
        ctx->index++;
        if (!closure) continue;  // deactivated mid-raise (#908)
        void* h_task = ctx->shim(closure, ctx->payload_buf);
        bz_task_then(h_task, bz_raise_chain_step, ctx);
        return;
    }
    // All handlers done — complete outer with success.
    bz_task_complete(ctx->outer, nullptr, 0);
    free(ctx->payload_buf);
    delete ctx;
}

static void bz_raise_chain_step(void* completed_task, void* state) {
    auto* ctx = (BzRaiseChainCtx*)state;
    if (bz_task_is_error(completed_task)) {
        // §24 first-error-wins: fault outer, skip remaining handlers.
        void* errObj = bz_task_error_obj(completed_task);
        bz_task_free(completed_task);
        bz_task_complete_error_obj(ctx->outer, errObj);
        free(ctx->payload_buf);
        delete ctx;
        return;
    }
    bz_task_free(completed_task);
    bz_raise_invoke_next(ctx);
}

// per §24 / §30.13 / #1377: non-blocking RaiseAsync entry point.
// srcp         — bz_event_source_t* (the AsyncEventSource backing field).
// payload_buf  — pointer to caller's stack buffer holding a copy of the payload;
//                null when payload_size == 0 (unit / no-payload event).
// payload_size — sizeof(payload_type) in bytes; 0 if no payload.
// shim_fn      — codegen-generated per-call-site function:
//                  void* shim(void* closure, void* payload_buf) → Task*
//                invokes the handler's fn(env, payload?) with the correct types.
// Returns the outer Task* that the caller's `await E.RaiseAsync(...)` awaits.
extern "C" void* bz_event_raise_async_chain(
        void* srcp, void* payload_buf, int64_t payload_size, void* shim_fn) {
    void* outer_raw = bz_task_alloc();
    auto* s = (bz_event_source_t*)srcp;
    int64_t n = s ? bz_event_count(s) : 0;
    if (n == 0) {
        // Zero subscribers: no-op; return an already-done task.
        bz_task_complete(outer_raw, nullptr, 0);
        return outer_raw;
    }
    // Heap-copy the payload so subsequent chain steps can read it after the
    // originating continuation (whose stack buffer holds the payload) returns.
    void* heap_payload = nullptr;
    if (payload_size > 0 && payload_buf) {
        heap_payload = malloc((size_t)payload_size);
        memcpy(heap_payload, payload_buf, (size_t)payload_size);
    }
    auto* ctx = new BzRaiseChainCtx{
        (bz_task_t*)outer_raw, s, n, 0,
        heap_payload, (bz_handler_shim_t)shim_fn
    };
    bz_raise_invoke_next(ctx);
    return outer_raw;
}

void* bz_string_split(const char* s, int64_t s_len,
                       const char* delim, int64_t delim_len) {
    void* arr = bz_array_alloc(8, 8);
    if (delim_len <= 0) {
        // Empty delimiter: return array with one element (the whole string)
        char* copy = (char*)malloc((size_t)(s_len + 1));
        memcpy(copy, s, (size_t)s_len);
        copy[s_len] = '\0';
        uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
        memcpy(strObj, &copy, sizeof(void*));
        memcpy(strObj + 8, &s_len, sizeof(int64_t));
        strObj[16] = 1;
        bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
        bz_array_add(arr, &strObj, 8);
        return arr;
    }
    int64_t start = 0;
    for (int64_t i = 0; i <= s_len - delim_len; ) {
        if (memcmp(s + i, delim, (size_t)delim_len) == 0) {
            int64_t partLen = i - start;
            char* part = (char*)malloc((size_t)(partLen + 1));
            memcpy(part, s + start, (size_t)partLen);
            part[partLen] = '\0';
            uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
            memcpy(strObj, &part, sizeof(void*));
            memcpy(strObj + 8, &partLen, sizeof(int64_t));
            strObj[16] = 1;
            bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
            bz_array_add(arr, &strObj, 8);
            i += delim_len;
            start = i;
        } else {
            i++;
        }
    }
    // Last segment
    int64_t partLen = s_len - start;
    char* part = (char*)malloc((size_t)(partLen + 1));
    memcpy(part, s + start, (size_t)partLen);
    part[partLen] = '\0';
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &part, sizeof(void*));
    memcpy(strObj + 8, &partLen, sizeof(int64_t));
    strObj[16] = 1;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    bz_array_add(arr, &strObj, 8);
    return arr;
}

// fix #4487 (stdlib §9.6 + spec §4.6, v0.306): `bz_string_compare` stood here.
// It took (data, len) for each operand and NOT the storage width, so it walked
// min(a_len, b_len) BYTES over a CODE-POINT length — the #535 defect class —
// and two different non-ASCII strings compared EQUAL ("日本".CompareTo("日x",
// true) was 0, measured on 4d9d186a). The width is not recoverable from that
// signature, so the helper could not be repaired in place; every caller had
// the string OBJECT in hand and now calls `bz_str_compare_cp`
// (bezel_runtime_strings.cpp), which decodes each operand at its own width via
// `bz_str_cp` and folds through the shared `bz_char_to_lower`. Deleting this
// one rather than leaving it beside its replacement is deliberate: a second
// "ordinal string comparison" that disagrees with the first is exactly the
// drift that caused #2590.

// per §9.6 Split(separators: Array<char>): split on any of `nsep` single-code-
// point separators. Width-1 (byte) mode, matching bz_string_split. Each emitted
// segment is a fresh +1 bz.string; the result array is owned by the caller.
static void bz__split_emit(void* arr, const char* s, int64_t start, int64_t end) {
    int64_t partLen = end - start;
    char* part = (char*)malloc((size_t)(partLen + 1));
    memcpy(part, s + start, (size_t)partLen);
    part[partLen] = '\0';
    uint8_t* strObj = (uint8_t*)calloc(1, (size_t)BZ_STRING_SIZEOF);
    memcpy(strObj, &part, sizeof(void*));
    memcpy(strObj + 8, &partLen, sizeof(int64_t));
    strObj[16] = 1;
    bz_string_init_rc(strObj);  // #422: producer's reference, rc := 1
    bz_array_add(arr, &strObj, 8);
}

void* bz_string_split_anychar(const char* s, int64_t s_len,
                              const int32_t* seps, int64_t nsep) {
    void* arr = bz_array_alloc(8, 8);
    // No separators (or empty set): the whole string is the single segment,
    // mirroring bz_string_split's empty-delimiter behaviour.
    if (nsep <= 0) {
        bz__split_emit(arr, s, 0, s_len);
        return arr;
    }
    int64_t start = 0;
    for (int64_t i = 0; i < s_len; ++i) {
        bool isSep = false;
        for (int64_t k = 0; k < nsep; ++k) {
            if ((int32_t)(uint8_t)s[i] == seps[k]) { isSep = true; break; }
        }
        if (isSep) {
            bz__split_emit(arr, s, start, i);
            start = i + 1;
        }
    }
    bz__split_emit(arr, s, start, s_len);  // trailing segment (possibly empty)
    return arr;
}
