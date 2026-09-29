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

// fix #5115 (§11.1 / §11.2, v0.313): dispatch through a generic interface
// at a concrete instantiation. A class listing the interface at two or more
// instantiations carries one itab per instantiation, keyed by that
// instantiation's descriptor and placed before its shared `iface_td` entry;
// every other implementor carries only the shared entry. One pass: an
// instantiation-keyed match wins, else the shared match.
extern "C" void** bz_itable_lookup_inst(void** itmap, void* inst_td,
                                        void* iface_td) {
    if (!itmap) return nullptr;
    void** shared = nullptr;
    for (void** p = itmap; *p; p += 2) {
        if (p[0] == inst_td) return (void**)p[1];
        if (!shared && p[0] == iface_td) shared = (void**)p[1];
    }
    return shared;
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

// §31.4/#811/#1389: reclaim a CauseChain snapshot at the exception object's
// TERMINAL reclaim (immediately before the object's own block is freed) —
// called from the codegen `emitDisposeAndReclaim` reclaim site
// (LlvmBackend_Disposal.cpp), never from the shared vtable Dispose
// (bz$exDisposeMsg). An exception object has several disposal/reclaim paths
// (catch-arm dispose, scope-end dispose, async fault propagation) that are
// not one idempotent site; earlier work tried freeing CauseChain@24 from the
// shared Dispose (mirroring how it releases Message/ParamName) and hit a
// use-after-free — a still-borrowed (not-yet-reclaimed) exception's
// CauseChain can be read (as a `cause`'s `nested` chain, via
// bz_exc_causechain_link below) AFTER its own Dispose has already dispatched
// on some path, so freeing it there raced that read. The reclaim site is
// different: ownership discipline runs it EXACTLY ONCE per object, strictly
// after every live borrow/read has concluded, so it cannot race an in-flight
// read.
//
// WHAT CARRIES THE SLOT. `excVtableTable` is the same null-terminated
// built-in-exception vtable table bz_unhandled_exception gates on: a directly
// constructed built-in exception's vtable is one of these. A USER subclass of
// a built-in exception (§31.4) carries the SAME §31.4 header (its layout
// mirrors Message@8/InnerException@16/CauseChain@24/ParamName@32 via
// hasBuiltinExceptionBase in classStruct), but its vtable is vt$<C>, which is
// NOT in excVtableTable — so the vtable-identity gate alone skipped it and its
// CauseChain leaked (issue #2989/#2979). The compiler already records that
// header-bearing fact in the type's descriptor chain: vtable[0] is td$<C>, a
// null-terminated array of the type and every ancestor, and setupBuiltinExceptions
// appends the built-in exception ancestor descriptors (btd$Exception, …) for
// exactly these classes — the SAME chain emitIsA walks for `o is Exception`.
// `excDescTable` is that built-in exception descriptor set; when the vtable is
// not a direct built-in, walk obj's descriptor chain and reclaim iff some
// ancestor is a built-in exception descriptor. This is the one authority the
// language already uses for the type test, not a second derivation; the
// offset-24 read stays gated behind it. Reading vtable[0] here is safe because
// emitDynamicDispose (run immediately above this call, LlvmBackend_Disposal.cpp)
// already dispatched vtable[2], i.e. obj[0] is a CG35-itable vtable whose [0]
// is its td$ descriptor — true of every class/box that reaches this site.
// The pure-runtime vtable-Dispose-then-free reclaim sites that bypass codegen
// (bz_dispose_class_obj, bz_domain_spawn's DomainFaultedException wrap) reach
// this same reclaim through bz_exc_reclaim_causechain_registered below, which
// supplies the tables from the program-start registration instead of from a
// codegen-emitted argument they have no access to (issue 2330).
extern "C" void bz_exc_reclaim_causechain(void* obj, void** excVtableTable,
                                          void** excDescTable) {
    if (!obj) return;
    void* vtable; memcpy(&vtable, obj, sizeof(void*));
    // A null vtable cannot be a built-in exception, and the descriptor walk
    // below would dereference it. The codegen reclaim site never passes one
    // (every object reaching it is CG35-itable-headed), but the runtime sites
    // added for issue 2330 explicitly tolerate a null vtable
    // (bz_dispose_class_obj guards its own Dispose dispatch on it), so the
    // guard belongs here rather than at each caller.
    if (!vtable) return;
    bool builtinExc = false;
    if (excVtableTable)
        for (void** p = excVtableTable; *p; ++p)
            if (*p == vtable) { builtinExc = true; break; }
    if (!builtinExc && excDescTable) {
        // §31.4/#2989: obj's vtable is vt$<C> (a user subclass) or a non-class
        // object. vtable[0] is td$<C>, a null-terminated ancestor-descriptor
        // array; reclaim iff a built-in exception descriptor is among them.
        void* desc; memcpy(&desc, vtable, sizeof(void*));
        if (desc) {
            for (void** d = (void**)desc; *d; ++d) {
                for (void** p = excDescTable; *p; ++p)
                    if (*d == *p) { builtinExc = true; break; }
                if (builtinExc) break;
            }
        }
    }
    if (!builtinExc) return;
    void* chain;
    memcpy(&chain, (uint8_t*)obj + BZ_EXC_CAUSE_OFFSET, sizeof(void*));
    if (!chain) return;
    auto* arr = (bz_array_t*)chain;
    auto* elems = (bz_excinfo_t*)arr->data;
    for (int64_t i = 0; i < arr->count; i++) {
        bz_string_release(elems[i].typeName);
        bz_string_release(elems[i].message);
        bz_string_release(elems[i].stackTrace);
    }
    free(arr->data);
    free(arr);
    // §28.2 exactly-once: null the slot after reclaiming it. Every caller frees
    // (or quarantines) the object immediately after this call, so the slot is
    // already unreachable in practice — but with the runtime sites of issue 2330
    // now sharing this reclaim, "exactly once" should not rest on each caller's
    // ordering: a second call on the same object returns at the `!chain` guard
    // above rather than double-freeing.
    void* null_chain = nullptr;
    memcpy((uint8_t*)obj + BZ_EXC_CAUSE_OFFSET, &null_chain, sizeof(void*));
}

// issue 2330 / §30.9 / §31.4: the built-in exception vtable + descriptor tables,
// registered once by the generated main prologue (emitMainStaticInitPrologue,
// LlvmBackend_Setup.cpp) before any user code runs.
//
// WHY A REGISTRATION rather than an argument. The codegen reclaim site passes
// these tables directly, because it is emitting code and has them in hand. The
// pure-runtime reclaim sites (bz_dispose_class_obj and bz_domain_spawn's
// DomainFaultedException wrap, bezel_runtime_concurrency.cpp) run inside task
// continuations with no codegen-supplied argument to carry them — which is
// exactly why they were left out of the original fix and leaked. Registering
// the tables at program start is the mechanism this runtime already uses for
// the same problem: bz_ser_exc_vtable_init (bezel_runtime.h) hands the
// SerializationException vtable to the runtime from the same prologue so
// bz_ser_throw can build a well-formed exception. This follows that precedent
// rather than inventing a second one.
//
// Both tables are module-level constants with program lifetime, and the
// registration runs on the main thread before any task or domain can spawn, so
// the later reads (which are the only other accesses) never race the write.
// btd$<name> is LinkOnceODR — one canonical address per type across modules —
// so the single registered descriptor table is authoritative program-wide.
static void** g_bz_exc_vtable_table = nullptr;
static void** g_bz_exc_desc_table = nullptr;

extern "C" void bz_exc_tables_init(void** excVtableTable, void** excDescTable) {
    g_bz_exc_vtable_table = excVtableTable;
    g_bz_exc_desc_table = excDescTable;
}

// The registered-table entry point for the pure-runtime reclaim sites. Before
// the tables are registered (or in a program whose entry point never ran) both
// are null, and bz_exc_reclaim_causechain's gates then reclaim nothing — the
// pre-issue-2330 behaviour, never a wrong free.
extern "C" void bz_exc_reclaim_causechain_registered(void* obj) {
    bz_exc_reclaim_causechain(obj, g_bz_exc_vtable_table, g_bz_exc_desc_table);
}

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
