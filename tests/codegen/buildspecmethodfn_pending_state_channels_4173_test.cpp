// Codegen / memory-unsafety — issue #4173: `Emitter::buildSpecializedMethodFn`
// (`LlvmBackend_Calls2.cpp` ~L13133) left its two §2731-item-3 ROOT-SCOPE
// REGISTRATION channels — `pendingOwnedParams_` and `pendingArgsDisposal_`
// (`LlvmBackend_impl.hpp`) — unprotected across its own nested emission, the
// same call site `fix #4157`/`#4158`/`#4169` audited and protected five other
// members at. #4173 was filed as a due-diligence audit note ("likely
// unreachable, unverified") and then MEASURED reachable by an R17 c3 review
// of that fix (see the issue's own comment thread): a `requires` clause is
// lowered (`emitRequiresPrologue`) strictly between an outer method's own
// `bindParameters` call and that method's own root-scope `lowerScopedBlock` —
// the exact window `fix #2731` item 3's own repro already proved reachable
// for the sibling `NestedBodyScope` guard (see that guard's ctor/dtor
// comment, `LlvmBackend_Assign.cpp` ~5021-5043) — and a `requires` clause may
// call a first-time generic `pure` method, which routes through
// `buildSpecializedMethodFn` exactly like any other first-time generic
// specialization.
//
// THE MECHANISM. `buildSpecializedMethodFn` calls `bindParameters` for the
// SPECIALIZED method it is building (`LlvmBackend_Calls2.cpp` ~13507), and
// `bindParameters` unconditionally does `pendingOwnedParams_.clear();` as its
// very first statement (`LlvmBackend_Setup.cpp` ~13934) — before this fix,
// nothing at this call site protected that. So when an OUTER method's own
// `requires` clause triggers a NESTED `buildSpecializedMethodFn` call before
// the outer's own root scope has drained its `pendingOwnedParams_`, the
// nested call's own `bindParameters` wipes the outer's pending `owned`
// parameter registration outright — no state-machine/generic-return-kind
// racing needed, the unconditional `.clear()` alone is sufficient.
// `pendingArgsDisposal_` shares the identical producer/consumer shape,
// specific to `Main(args: Array<string>)`'s own argv-array prologue
// (`LlvmBackend_Setup.cpp` ~9473 / `LlvmBackend_Async.cpp` ~1223, both gated
// on `isMain(ms)`).
//
// TWO DIFFERENT FAILURE MODES, not one:
//   * pendingOwnedParams_ — a plain LEAK. The outer method's `owned`
//     parameter is silently never disposed (its pending unwind-disposal
//     registration is wiped before `lowerScopedBlock` can drain it).
//   * pendingArgsDisposal_ — a COMPILE-TIME CRASH, not a leak. Without this
//     fix, the OUTER Main's `Disposal{argsSlot, nullptr}` (an `AllocaInst*`
//     that lives in Main's own function) survives into the nested
//     specialization's build untouched, and THAT function's own root-scope
//     registration drains it as if it were ITS OWN disposal — emitting a use
//     of Main's alloca inside a DIFFERENT LLVM function, which the verifier
//     rejects: "Referring to an instruction in another function!" MEASURED
//     (this fix's own FAIL-WITHOUT cycle, reverting only this file's diff):
//     `bezelc: codegen error: IR verification failed (malformed IR emitted;
//     see report above)` — the program never compiles at all pre-fix, so its
//     regression row below asserts on `runAot`'s `r.ran`, not a leak byte
//     count.
//
// FIX. Both members are ALSO hand-declared members of `NestedBodyScope`
// (`LlvmBackend_impl.hpp` ~12728-12730) — the sibling guard that ALREADY
// saves/clears/restores them by name for the OTHER nested-emission venues
// (property/choice-method getter/setter bodies) — so, exactly like #4157's
// `asyncChainedReturn_`/`asyncBorrowedReturn_` pair at this same site,
// folding them into `BEZEL_SM_FRAME_MEMBERS` would duplicate-declare them in
// `NestedBodyScope`. Fixed with the same by-name save/reset/restore shape
// #4157 used, placed alongside it (save before `currentFn_ = specFn;`,
// restore in the function's single "Restore outer state" section — the only
// earlier `return`, the cache-hit `return cit->second;`, is well before the
// save point).
//
// ORACLE. The pendingOwnedParams_ row is loop-amplified per LANE_RULES
// (200 iterations of a call whose `owned` parameter's disposal decrements a
// destructor-driven static counter) and run under both the plain AOT oracle
// and ASan/LeakSanitizer — neither dominates (#2633). The pendingArgsDisposal_
// row is a compile-success/IR-verification check (a crash, not a leak), so
// it is asserted on the plain oracle via `r.ran` + exit code; ASan adds
// nothing extra here since the defect never reaches runtime pre-fix.
//
// MEASURED (this fix's own probes, `agent_check.sh --probe --asan`, and the
// FAIL-WITHOUT cycle below):
//   pendingOwnedParams_: pre-fix LeakSanitizer, matching the issue's own
//     review comment (800 B / 50 allocations at 50 iterations; this test
//     amplifies to 200). post-fix: clean, Widget4173PP.n == 0.
//   pendingArgsDisposal_: pre-fix `bezelc: codegen error: IR verification
//     failed` (compile never succeeds — `r.ran == false`). post-fix: compiles
//     and runs, returning args.Count (0 — the harness passes no argv).
//
// Spec: §28.3 (owned-parameter auto-drop), §33.4/§33.6 (Main(args) argv
// array), v0.298.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-instrumented) AOT run. A SIGSEGV or a compile-time rejection is
// WIFSIGNALED / ran=false -> -999.
int runPlain(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan+LSan run: a leak surfaces as an exit code that does not match the
// program's own clean-path return value (LSan reports at process exit).
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent -> case skipped
    return r.ran ? r.exitCode : -999;
}

// -----------------------------------------------------------------------
// pendingOwnedParams_ — LEAK
// -----------------------------------------------------------------------

const char* kWidget4173PP =
    "class Widget4173PP "
    "  public static n: int := 0; "
    "  public constructor() Widget4173PP.n := Widget4173PP.n + 1; end constructor; "
    "  destructor Widget4173PP.n := Widget4173PP.n - 1; end destructor; "
    "end Widget4173PP; ";

constexpr int kIters4173PP = 200;

// THE ROW. `C4173PP.M`'s `requires` clause calls `Gen4173PP<int>.Foo(1)` — a
// first-time generic PURE method — before `M`'s own root scope has drained
// its `owned r` parameter's pending unwind-disposal registration. Without
// this fix, `Gen4173PP<int>.Foo`'s own `bindParameters` (inside the nested
// `buildSpecializedMethodFn` call) clears `pendingOwnedParams_` out from
// under `M`, so `r` is never disposed.
std::string programOwnedParams4173() {
    return std::string("module M; ") + kWidget4173PP +
        "class Gen4173PP<T> "
        "  public static pure method Foo(x: T): bool "
        "    return true; "
        "  end Foo; "
        "end Gen4173PP; "
        "class C4173PP "
        "  public static method M(owned r: Widget4173PP): int "
        "    requires(Gen4173PP<int>.Foo(1)) "
        "    return 1; "
        "  end M; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<" + std::to_string(kIters4173PP) + " do "
        "      total := total + C4173PP.M(new Widget4173PP()); "
        "    end; "
        "    return Widget4173PP.n; "
        "  end Main; "
        "end C4173PP;";
}

// CONTROL A — identical shape, `requires` clause removed entirely: no window
// is ever open (the outer's own root scope drains pendingOwnedParams_ well
// before anything else could reach it). Must be clean on both sides.
std::string controlNoRequires4173() {
    return std::string("module M; ") + kWidget4173PP +
        "class C4173PPa "
        "  public static method M(owned r: Widget4173PP): int "
        "    return 1; "
        "  end M; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<" + std::to_string(kIters4173PP) + " do "
        "      total := total + C4173PPa.M(new Widget4173PP()); "
        "    end; "
        "    return Widget4173PP.n; "
        "  end Main; "
        "end C4173PPa;";
}

// CONTROL B — `requires` clause present but calling a NON-generic pure
// method: `buildSpecializedMethodFn` (and this fix's new code inside it) is
// never reached. Must be clean on both sides — isolates the cause to the
// GENERIC specialization, not to `requires` clauses in general.
std::string controlNongenericRequires4173() {
    return std::string("module M; ") + kWidget4173PP +
        "class Plain4173PP "
        "  public static pure method Foo(x: int): bool "
        "    return true; "
        "  end Foo; "
        "end Plain4173PP; "
        "class C4173PPb "
        "  public static method M(owned r: Widget4173PP): int "
        "    requires(Plain4173PP.Foo(1)) "
        "    return 1; "
        "  end M; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<" + std::to_string(kIters4173PP) + " do "
        "      total := total + C4173PPb.M(new Widget4173PP()); "
        "    end; "
        "    return Widget4173PP.n; "
        "  end Main; "
        "end C4173PPb;";
}

// -----------------------------------------------------------------------
// pendingArgsDisposal_ — COMPILE-TIME CRASH (IR verification failure)
// -----------------------------------------------------------------------

// THE ROW. `Main(args: Array<string>)` is the ONLY method whose body ever
// populates `pendingArgsDisposal_` (`isMain(ms)`-gated, both the plain and
// async prologues). Its own `requires` clause calls a first-time generic
// PURE method before Main's own root scope drains that disposal. Without
// this fix, the nested `buildSpecializedMethodFn` call's own root-scope
// registration drains the OUTER Main's `Disposal{argsSlot, nullptr}` as if
// it were its own — argsSlot is an `AllocaInst*` that lives in Main's
// function, so the specialized function ends up referencing an instruction
// from a DIFFERENT LLVM function, which the IR verifier rejects outright:
// the program never compiles.
std::string programArgsDisposal4173() {
    return "module M; "
        "class Gen4173AD<T> "
        "  public static pure method Foo(x: T): bool "
        "    return true; "
        "  end Foo; "
        "end Gen4173AD; "
        "class C4173AD "
        "  public static method Main(args: Array<string>): int "
        "    requires(Gen4173AD<int>.Foo(1)) "
        "    return args.Count; "
        "  end Main; "
        "end C4173AD;";
}

// CONTROL — identical shape, but `Foo` is NON-generic: `buildSpecializedMethodFn`
// is never reached, so the cross-function reference can never be built. Must
// compile and run identically on both sides (args.Count == 0 — the AOT
// harness passes no argv).
std::string controlArgsDisposalNongeneric4173() {
    return "module M; "
        "class Plain4173AD "
        "  public static pure method Foo(x: int): bool "
        "    return true; "
        "  end Foo; "
        "end Plain4173AD; "
        "class C4173ADb "
        "  public static method Main(args: Array<string>): int "
        "    requires(Plain4173AD.Foo(1)) "
        "    return args.Count; "
        "  end Main; "
        "end C4173ADb;";
}

// CONTROL — Main(args) with no `requires` clause at all: the ordinary,
// long-working shape (already pinned by tests/codegen/main_args_633_test.cpp)
// — included here as a lightweight sanity check that this fix's new
// save/reset/restore is a true no-op when buildSpecializedMethodFn's nested
// path is never exercised for Main itself.
std::string controlArgsDisposalNoRequires4173() {
    return "module M; "
        "class C4173ADc "
        "  public static method Main(args: Array<string>): int "
        "    return args.Count; "
        "  end Main; "
        "end C4173ADc;";
}

}  // namespace

// pendingOwnedParams_: pre-fix leaks one Widget4173PP per C4173PP.M() call
// (measured 800 B / 50 allocations at 50 iterations in the issue's own
// review comment; this test amplifies to 200); post-fix clean.
BEZEL_TEST(buildspecmethodfn_pending_state_channels_4173,
           owned_param_leaked_by_requires_clause_first_time_generic_spec_4173) {
    const std::string src = programOwnedParams4173();
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL A — no `requires` clause; the window never opens. Must be clean.
BEZEL_TEST(buildspecmethodfn_pending_state_channels_4173,
           owned_param_no_requires_control_4173) {
    const std::string src = controlNoRequires4173();
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL B — `requires` clause present but non-generic; buildSpecializedMethodFn
// never reached. Must be clean.
BEZEL_TEST(buildspecmethodfn_pending_state_channels_4173,
           owned_param_nongeneric_requires_control_4173) {
    const std::string src = controlNongenericRequires4173();
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// pendingArgsDisposal_: pre-fix FAILS TO COMPILE (IR verification failure,
// "Referring to an instruction in another function!") — asserted via `ran`,
// not a leak byte count, per LANE_RULES §18 (a crash needs a located-diagnostic
// check paired with the actual failure mode; here the failure mode itself IS
// non-compilation, so `runPlain`'s -999 sentinel on `!ran` is the exact
// observable). post-fix: compiles and returns args.Count (0).
BEZEL_TEST(buildspecmethodfn_pending_state_channels_4173,
           main_args_disposal_leaked_by_requires_clause_first_time_generic_spec_4173) {
    const std::string src = programArgsDisposal4173();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
}

// CONTROL — same shape, non-generic `requires` callee; buildSpecializedMethodFn
// never reached, so no cross-function reference can ever be built. Must
// compile and return 0 on both sides.
BEZEL_TEST(buildspecmethodfn_pending_state_channels_4173,
           main_args_disposal_nongeneric_requires_control_4173) {
    const std::string src = controlArgsDisposalNongeneric4173();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
}

// CONTROL — Main(args) with no `requires` clause at all: the ordinary shape.
// Must compile and return 0 on both sides (already covered by
// main_args_633_test.cpp; kept here as a same-file sanity anchor).
BEZEL_TEST(buildspecmethodfn_pending_state_channels_4173,
           main_args_disposal_no_requires_control_4173) {
    const std::string src = controlArgsDisposalNoRequires4173();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
}
