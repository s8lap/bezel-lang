// bezel_runtime_itable.cpp — per-interface itable lookup, exception-info helpers, bz_exc slot.
// Split from bezel_runtime.cpp per #1414 stage 2 (behavior-preserving motion).
#include "bezel_runtime_internal.hpp"

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// CG35-itable / §13: per-interface itable lookup.
// `itmap` points to a null-terminated flat array
//   [ td*I1, itab*I1, td*I2, itab*I2, ..., NULL ]
// stored in vtable[1] of every class/box vtable. Walk pairs until a td*
// matches `iface_td` and return the adjacent itab* pointer. For a
// well-typed Bezel program this always succeeds (the implementing class
// must have the interface in its itmap); returning NULL here is unreachable.
extern "C" void** bz_itable_lookup(void** itmap, void* iface_td) {
    if (!itmap) return nullptr;
    for (void** p = itmap; *p; p += 2) {
        if (p[0] == iface_td) return (void**)p[1];
    }
    return nullptr;  // unreachable for a typed program
}

// §31.4 (v0.162/v0.163) built-in exception object layout — byte offsets that
// must match the codegen constants in LlvmBackend_impl.hpp (kExc*Offset):
//   [0] vtable  [8] Message  [16] InnerException(TIE)  [24] CauseChain
//   [32] ParamName
static const int64_t BZ_EXC_MSG_OFFSET   = 8;
static const int64_t BZ_EXC_CAUSE_OFFSET = 24;

// §31.4 ExceptionInfo record — the {TypeName, Message, StackTrace} value type
// codegen lowers to three consecutive bz_string* (a 24-byte {ptr,ptr,ptr}
// record), stored inline in the Array<ExceptionInfo> CauseChain.
struct bz_excinfo_t { void* typeName; void* message; void* stackTrace; };

// A process-lifetime, shared empty bz_string carrying the SENTINEL rc so
// retain/release are no-ops and it is never freed. Used for the §31.4 StackTrace
// stub ("") and to normalise a null Message to "" inside a snapshot.
static void* bz_rt_empty_string() {
    static struct EmptyStr {
        uint8_t buf[BZ_STRING_SIZEOF];
        EmptyStr() {
            memset(buf, 0, sizeof(buf));
            buf[16] = 1;  // width = 1 (UTF-8 bytes); data=null, len=0
            int64_t sentinel = BZ_STRING_RC_SENTINEL;
            memcpy(buf + BZ_STRING_RC_OFFSET, &sentinel, sizeof(int64_t));
        }
    } e;
    return e.buf;
}

// §31.4 / #811 snapshot-chaining: build a fresh Array<ExceptionInfo> for the
// (message, cause) constructor. The new chain is
//   [ {causeTypeName, causeMessage, ""} ]  ++  nested
// where `nested` is the cause's own CauseChain (or null) — outermost cause
// first, flattened. Every stored string is bz_string_retain'd so the snapshot
// survives the borrowed cause's disposal (§31.1: the cause is never retained as
// an instance). The snapshot array itself is reclaimed at process exit (see the
// disposal note above — it is not freed per-exception to stay crash-safe across
// the multiple exception disposal/reclaim paths).
extern "C" void* bz_exc_causechain_link(void* causeTypeName, void* causeMessage,
                                        void* nested) {
    auto* nchain = (bz_array_t*)nested;
    int64_t ncount = nchain ? nchain->count : 0;
    auto* arr = (bz_array_t*)bz_array_alloc_count_n(
        (int64_t)sizeof(bz_excinfo_t), 1 + ncount);
    auto* elems = (bz_excinfo_t*)arr->data;
    void* empty = bz_rt_empty_string();
    void* tn = causeTypeName ? causeTypeName : empty;
    void* mg = causeMessage ? causeMessage : empty;
    bz_string_retain(tn);
    bz_string_retain(mg);
    elems[0].typeName   = tn;
    elems[0].message    = mg;
    elems[0].stackTrace = empty;   // §31.4 stub — "" pre-debug-info
    if (nchain) {
        auto* src = (bz_excinfo_t*)nchain->data;
        for (int64_t i = 0; i < ncount; i++) {
            void* st = src[i].stackTrace ? src[i].stackTrace : empty;
            bz_string_retain(src[i].typeName);
            bz_string_retain(src[i].message);
            bz_string_retain(st);
            elems[1 + i].typeName   = src[i].typeName;
            elems[1 + i].message    = src[i].message;
            elems[1 + i].stackTrace = st;
        }
    }
    return arr;
}

// §31.4/#811 note: a CauseChain snapshot is intentionally NOT freed when its
// owning exception is disposed. An exception object has several disposal/reclaim
// paths (catch-arm dispose, scope-end dispose, and the async fault-propagation
// free in bz$async$*$Resume) that are not one idempotent site, so a Dispose that
// freed CauseChain@24 double-freed it (and read freed memory) on the async path.
// The CauseChain is therefore reclaimed by process exit, exactly like the
// runtime-owned TypeInitializationException.InnerException (§22.13). A small,
// bounded leak confined to a caught-and-disposed exception constructed WITH a
// cause; see the codegen note in setupBuiltinExceptions.

// Write a bz_string's UTF-8 bytes to `f` (nothing for a null / empty string).
static void bz_rt_fput_bzstring(void* s, FILE* f) {
    if (!s) return;
    char* data; int64_t len;
    memcpy(&data, (uint8_t*)s, sizeof(void*));         // data @ 0
    memcpy(&len,  (uint8_t*)s + 8, sizeof(int64_t));   // len  @ 8
    if (data && len > 0) fwrite(data, 1, (size_t)len, f);
}

static int64_t bz_rt_strlen(void* s) {
    if (!s) return 0;
    int64_t len; memcpy(&len, (uint8_t*)s + 8, sizeof(int64_t));
    return len;
}

// per §33.3 / §22.13 (line 3845) / #1366: the runtime's interception point for
// an exception that escapes Main with no enclosing handler. Codegen emits a call
// to this from Main's top-level no-handler propagation block (in place of the
// old `ret i32 0`, which conflated abnormal termination with normal completion /
// `return 0`), passing the in-flight exception object. §33.3 (v0.163) requires
// the runtime to print the exception's type, message, stack trace, and
// CauseChain (§31.4) to stderr, then exit 70 (EX_SOFTWARE, BSD sysexits.h). The
// exact format string is implementation-defined; type/message/chain content is
// normative. The type name is the exception's virtual ToString (vtable slot 3),
// which for a built-in exception is the §8.5 default — its type name.
extern "C" void bz_unhandled_exception(void* exc, void** excVtableTable) {
    FILE* f = stderr;
    if (!exc) {  // defensive: no in-flight object (should not happen)
        fputs("Unhandled exception\n", f);
        // fix #1609 / §33.3: use _exit, not exit — see the note at the §33.3
        // exit site below (an uncaught fault leaves the scheduler pool's worker
        // threads running; exit()'s atexit/static-dtor phase can deadlock or
        // race against them, so terminate without running them).
        fflush(f);
        _exit(70);
    }
    // Virtual ToString (vtable slot 3) is safe for any class instance — the type
    // name (§8.5 default for a built-in exception). The Message@8 / CauseChain@24
    // reads have the §31.4 built-in layout only; gate them on the object's vtable
    // being one of the built-in exception vtables so a user class thrown uncaught
    // (arbitrary layout / size) is reported by type name alone, never by an
    // out-of-bounds field read.
    void* vtable; memcpy(&vtable, exc, sizeof(void*));
    bool builtinExc = false;
    if (excVtableTable) {
        for (void** p = excVtableTable; *p; ++p)
            if (*p == vtable) { builtinExc = true; break; }
    }
    void* toStrFnP; memcpy(&toStrFnP, (uint8_t*)vtable + 3 * sizeof(void*),
                           sizeof(void*));
    typedef void* (*bz_tostr_fn)(void*);
    void* typeName = toStrFnP ? ((bz_tostr_fn)toStrFnP)(exc) : nullptr;

    fputs("Unhandled exception", f);
    if (bz_rt_strlen(typeName) > 0) { fputs(": ", f); bz_rt_fput_bzstring(typeName, f); }
    if (builtinExc) {
        void* message; memcpy(&message, (uint8_t*)exc + BZ_EXC_MSG_OFFSET,
                              sizeof(void*));
        if (bz_rt_strlen(message) > 0) {
            fputs(": ", f);
            bz_rt_fput_bzstring(message, f);
        }
    }
    fputc('\n', f);

    // §31.4 CauseChain (already snapshotted at construction) — render each record
    // outermost-cause-first. Only a built-in exception has the CauseChain slot.
    if (builtinExc) {
        void* chain;
        memcpy(&chain, (uint8_t*)exc + BZ_EXC_CAUSE_OFFSET, sizeof(void*));
        if (chain) {
            auto* arr = (bz_array_t*)chain;
            auto* elems = (bz_excinfo_t*)arr->data;
            for (int64_t i = 0; i < arr->count; i++) {
                fputs("Caused by: ", f);
                bz_rt_fput_bzstring(elems[i].typeName, f);
                if (bz_rt_strlen(elems[i].message) > 0) {
                    fputs(": ", f);
                    bz_rt_fput_bzstring(elems[i].message, f);
                }
                fputc('\n', f);
            }
        }
    }
    // fix #1609 / §33.3: terminate with _exit, NOT exit. This is the process's
    // abnormal-termination boundary for an uncaught exception. Reaching it from
    // an async Main (or any program that spawned domains) leaves the scheduler
    // pool's worker threads — and possibly the timer thread — still running: the
    // fault path completes the outer task and unwinds to `main`, but never calls
    // bz_scheduler_shutdown (that runs only on normal completion). exit() would
    // then run atexit handlers and C++ static destructors on THIS thread while
    // those worker threads are live and may hold a libc lock (malloc arena,
    // stdio) or be blocked in a system call — a deadlock (observed as a
    // multi-minute hang under load) or data race. _exit terminates immediately
    // without that teardown; stdio is flushed explicitly first so the §33.3
    // report above is not lost. Matches C++ std::terminate's abort-style exit
    // discipline for an already-failing process.
    fflush(f);
    _exit(70);
}

// fix #492 / §31.1 / §2.6: the per-thread in-flight exception pointer.
// Codegen emits ExternalLinkage references to "bz_exc" in every compiled module
// (library and main); the linker merges them all to this one definition.
// Null = no exception in flight; non-null = heap-allocated exception object.
// The symbol name is "bz_exc" (valid C identifier, extern "C" suppresses
// C++ name mangling). The previous per-module name "bz$exc" was InternalLinkage
// (BSS) — the $ was invisible externally and is dropped now that the symbol is
// external.
//
// fix #704 / §30.14: made thread_local so each domain worker thread has its own
// independent exception slot.  Before this fix, two concurrent Domain.Spawn
// workers racing on the single process-wide slot could clobber each other's
// in-flight exception: one worker's "bz_exc = nullptr" cleared the other's
// just-thrown exception, causing lost faults or incorrect fault detection.
// C++ allows extern "C" thread_local; the symbol name "bz_exc" is preserved so
// generated-code references that go through bz_exc_addr() still access the
// correct per-thread slot.  Single-domain / single-thread programs are
// unaffected: one TLS slot per thread behaves identically to the old single
// global in the one-thread case.
extern "C" thread_local void* bz_exc = nullptr;

// fix #704: bz_exc_addr() returns a pointer to the calling thread's bz_exc TLS
// slot.  Codegen emits `call ptr @bz_exc_addr()` followed by a load or store
// through the returned pointer, rather than referencing bz_exc as a TLS
// GlobalVariable.  This avoids the LLVM ORC JIT incompatibility: the JIT
// generates emulated-TLS code (looking for "__emutls_v.bz_exc") for any
// thread_local GlobalVariable, but the host process only exports the real TLS
// symbol "bz_exc".  Since bz_exc_addr() is a plain extern "C" function, the
// JIT resolves it via DynamicLibrarySearchGenerator and calls into the runtime,
// which then accesses the real TLS slot directly.
extern "C" void** bz_exc_addr() { return &bz_exc; }

// fix #713: return the g_current_domain value for the calling thread.
// Used by tests to verify domain_drain sets the tracker; not called from
// generated code.  Returns a void* (opaque handle) — tests compare with
// domain pointers obtained from bz_domain_spawn's join_task->domain, but
// more directly we expose it as void* to avoid exposing bz_domain_t.
// per stdlib §6.8 (Domain): Domain.Spawn attaches work to a per-domain
// runtime context; the per-thread current-domain pointer is the §30.13
// serial-per-domain dispatch carrier (see concurrency.cpp's bz_domain_t
// and `g_current_domain`).
extern "C" void* bz_current_domain_ptr() { return (void*)g_current_domain; }
