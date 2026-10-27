// Codegen — a bare `owned` local/param transferred into an `owned` argument is
// disposed EXACTLY ONCE on the CAUGHT arg-throw path (issues #1990 & #1992,
// §28.3 "Exception-path discharge").
//
// ROOT CAUSE: a bare owned local/param `x` passed as `owned x` has its
// auto-dispose registered by registerUnwindOwnedLocal / registerUnwindOwnedParam
// in the scope where it was DECLARED (the whole-method / enclosing block). The
// #1918 machinery relies on that registration firing on a later-argument throw,
// nulling the caller slot only after the whole list is evaluated. That works
// when the throw PROPAGATES out of the method — but when the transfer site sits
// inside a `try` whose `catch` is in the SAME enclosing scope, a throw from a
// LATER argument unwinds only to the in-method handler; it does NOT exit the
// declaring scope, so the declaring-scope registration never fires. And because
// sema treats `x` as moved past the `owned` transfer, its normal-exit discharge
// is suppressed too. The still-owned value is therefore NEVER disposed on the
// caught path:
//   #1990 — one transferred owned local leaks (surfaced by a post-catch
//           conditional return, which perturbs the stack enough that LSan's
//           conservative scan stops hiding the leak);
//   #1992 — with N>=2 transferred owned args, all-but-the-last leak (the last
//           happened to be reclaimed on the base by the #1968 block free path;
//           each is really un-disposed).
//
// FIX (src/codegen/LlvmBackend_Assign.cpp, buildOrderedArgs): for each bare
// owned local/param transferred via `owned x`, register a TRANSIENT unwind-only
// disposal on the local's OWN slot in the CURRENT scope (disposeStack_.back() —
// the scope the later argument's emitPendingCheck unwinds through on BOTH the
// caught and propagate edges), mirroring the #993 owned-TEMP handling. On the
// caught arg-throw it disposes the value exactly once; on a propagate throw it
// disposes + NULLS the slot so the declaring-scope disposal (null-guarded) then
// skips (no double-free); on the normal transfer-completes path it is unwind-
// only (never fires on a normal exit) and the slot is nulled, so it is inert.
// Restricted to a vtable-dispatchable user class/interface instance. Disjoint
// from the #1968 newArgUnwindCleanup, which raw-frees only the uninitialized
// instance BLOCK.
//
// WITNESS (stronger than LSan, which conservative-stack-scans and can FALSELY
// report a leaked owned local as clean): each destructor bumps a static counter,
// and Main READS that counter AFTER the catch — the transient disposal fires on
// the arg-throw edge, before the post-catch code, so the count is observable.
// Each test also runs under runAotChecked (ASan/LSan), so a leak, double-free,
// or use-after-free additionally FAILS via r.ran=false. fails-without/passes-with
// verified: on the base every witness returns non-zero (X._n under-counts).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// T1 — #1990 exact repro: a single bare owned local `x` transferred into
// `new Pair(move x, <throws>)`, the throw CAUGHT in-method, with a post-catch
// conditional `return`. Witness: after the catch, `x` must have been disposed
// EXACTLY ONCE (X._n == 1). (On the base the destructor never ran → X._n == 0 →
// return 3; and LSan caught the 16-byte leak.)
const char* kSingleOwnedCaughtPostCatchReturn =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class Pair "
    "  public owned _x: X; "
    "  public _b: int; "
    "  public constructor(owned x: X, b: int) _x := x; _b := b; end constructor; "
    "  destructor end destructor; "
    "end Pair; "
    "class P "
    "  public static method MakeIntOrThrow(): int "
    "    throw new Exception(\"boom\"); "
    "  end MakeIntOrThrow; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var x := new X(); "
    "    try using var p := new Pair(move x, P.MakeIntOrThrow()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    if X._n <> 1 then return 3; end; "       // x disposed once on the caught edge
    "    return 0; "
    "  end Main; "
    "end P;";

// T2 — #1992: TWO bare owned locals of the SAME type transferred into
// `new Trip(move x, move y, <throws>)`, the throw CAUGHT. Witness: BOTH must
// be disposed once (X._n == 2). (On the base only one was reclaimed → X._n == 0
// witnessed here, and LSan caught the surviving 16-byte leak.)
const char* kTwoOwnedSameTypeCaught =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class Trip "
    "  public owned _x: X; "
    "  public owned _y: X; "
    "  public _b: int; "
    "  public constructor(owned x: X, owned y: X, b: int) "
    "    _x := x; _y := y; _b := b; end constructor; "
    "  destructor end destructor; "
    "end Trip; "
    "class P "
    "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var x := new X(); "
    "    var y := new X(); "
    "    try using var t := new Trip(move x, move y, P.Boom()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    if X._n <> 2 then return 3; end; "       // BOTH disposed once
    "    return 0; "
    "  end Main; "
    "end P;";

// T3 — #1992 with TWO DIFFERENT owned types, so each destructor has its own
// counter: proves it is BOTH args (not one disposed twice) that are reclaimed —
// X._n == 1 AND Y._n == 1 — and LSan proves neither leaks nor double-frees.
const char* kTwoOwnedDistinctTypesCaught =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class Y "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Y._n := Y._n + 1; end destructor; "
    "end Y; "
    "class Trip "
    "  public owned _x: X; "
    "  public owned _y: Y; "
    "  public _b: int; "
    "  public constructor(owned x: X, owned y: Y, b: int) "
    "    _x := x; _y := y; _b := b; end constructor; "
    "  destructor end destructor; "
    "end Trip; "
    "class P "
    "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var x := new X(); "
    "    var y := new Y(); "
    "    try using var t := new Trip(move x, move y, P.Boom()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    if X._n <> 1 then return 3; end; "       // x disposed once
    "    if Y._n <> 1 then return 4; end; "       // y disposed once
    "    return 0; "
    "  end Main; "
    "end P;";

// T4 — SUCCESSFUL construction (no throw): the ctor takes ownership of both
// args; `t` is disposed at its scope end, so each owned arg is disposed EXACTLY
// once via Trip's owned fields — the transient MUST be inert here (no double-free
// with the ctor's owned-field disposal, no premature free). Witness: after t's
// scope, X._n == 2.
const char* kSuccessfulTwoOwned =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class Trip "
    "  public owned _x: X; "
    "  public owned _y: X; "
    "  public _b: int; "
    "  public constructor(owned x: X, owned y: X, b: int) "
    "    _x := x; _y := y; _b := b; end constructor; "
    "  destructor end destructor; "
    "end Trip; "
    "class P "
    "  public static method Ident(v: int): int return v; end Ident; "
    "  public static method Main(): int "
    "    var x := new X(); "
    "    var y := new X(); "
    "    do "
    "      using var t := new Trip(move x, move y, P.Ident(7)); "
    "      if t._b <> 7 then return 4; end; "
    "    end; "                                   // t disposed here → X._n == 2
    "    if X._n <> 2 then return 5; end; "
    "    return 0; "
    "  end Main; "
    "end P;";

// T5 — the same defect on the shared buildOrderedArgs path used by an ORDINARY
// method call: `P.Sink(move x, move y, <throws>)` caught in-method. Both
// transferred owned args are disposed exactly once on the caught edge. Sink
// disposes its owned params on the (untaken here) normal path so the program is
// well-formed. Witness: X._n == 2 after the catch.
const char* kMethodCallTwoOwnedCaught =
    "module M; "
    "class X "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor X._n := X._n + 1; end destructor; "
    "end X; "
    "class P "
    "  public static method Sink(owned a: X, owned b: X, n: int): int "
    "    a.Dispose(); b.Dispose(); return n; end Sink; "
    "  public static method Boom(): int throw new Exception(\"boom\"); end Boom; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    var x := new X(); "
    "    var y := new X(); "
    "    try var r := P.Sink(move x, move y, P.Boom()); "
    "    catch Exception => caught := 1; end; "
    "    if caught <> 1 then return 2; end; "
    "    if X._n <> 2 then return 3; end; "       // both disposed once on the caught edge
    "    return 0; "
    "  end Main; "
    "end P;";

}  // namespace

// ── T1: #1990 — single owned local, caught, post-catch conditional return ─────
BEZEL_TEST(new_ctor_owned_arg_unwind_leak_1990_1992,
           single_owned_caught_post_catch_return_disposed_once_spec_28_3) {
    JitResult r = runAotChecked(kSingleOwnedCaughtPostCatchReturn);
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: owned x disposed once, not leaked
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // caught AND X._n==1 (witness)
}

// ── T2: #1992 — two owned args (same type), caught: BOTH disposed once ────────
BEZEL_TEST(new_ctor_owned_arg_unwind_leak_1990_1992,
           two_owned_same_type_caught_both_disposed_once_spec_28_3) {
    JitResult r = runAotChecked(kTwoOwnedSameTypeCaught);
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: neither owned arg leaks/double-frees
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // caught AND X._n==2 (witness)
}

// ── T3: #1992 — two owned args (distinct types), caught: each disposed once ───
BEZEL_TEST(new_ctor_owned_arg_unwind_leak_1990_1992,
           two_owned_distinct_types_caught_each_disposed_once_spec_28_3) {
    JitResult r = runAotChecked(kTwoOwnedDistinctTypesCaught);
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean on both distinct owned args
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // caught AND X._n==1 AND Y._n==1 (witness)
}

// ── T4: §28.3 — successful construction: transient inert, no double-free ──────
BEZEL_TEST(new_ctor_owned_arg_unwind_leak_1990_1992,
           successful_two_owned_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kSuccessfulTwoOwned);
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: constructed + disposed once each
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // X._n==2 at t's scope end (witness)
}

// ── T5: #1992 sibling — the same fix on an ORDINARY method-call arg list ───────
BEZEL_TEST(new_ctor_owned_arg_unwind_leak_1990_1992,
           method_call_two_owned_caught_both_disposed_once_spec_28_3) {
    JitResult r = runAotChecked(kMethodCallTwoOwnedCaught);
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean on the method-call path
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // caught AND X._n==2 (witness)
}
