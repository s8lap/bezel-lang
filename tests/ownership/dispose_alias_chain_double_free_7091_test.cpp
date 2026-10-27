// Spec-derived regression tests for issue #7091 — the §28.6 borrow-receiver
// check read the receiver's alias source ONE hop only, so a `Dispose()`
// through a TWO-hop alias chain compiled with NO diagnostic and double-freed
// the collection at run time.
//
// THE DEFECT (issue #7091, measured by the coordinator on the staging build
// 9f1d7603 and re-measured on this lane's HEAD):
//
//   var a: Array<int> := new Array<int>(2);
//   var b: Array<int> := a;
//   var il: IList<int> := b;
//   il.Dispose();
//
// compiled with NO diagnostic and then aborted under generated-code
// AddressSanitizer with "ERROR: AddressSanitizer: attempting double-free on
// 0x7b70373e0010 in thread T0: #1 bz_array_free
// src/runtime/bezel_runtime_collections.cpp:741:5 #2 main" — the explicit
// `il.Dispose()` (the interface arm of the explicit-dispose chain) and `a`'s
// scope-end pass, on the same 8-byte region.
//
// THE SPEC SENTENCE THAT DECIDES THE OUTCOME — §28.3 "Ownership state of
// locals", *Consuming positions for an owned-local read* (v0.340):
//
//   "an **unmarked** local binding or assignment RHS over a **class- or
//    interface-typed** binding — `var y := x;`, `y := x;`: a plain alias
//    binding is a borrow of `x`, source `x` itself"
//
// So `b` is a borrow of `a`, and `il` is a borrow of that borrow. §28.6 "Who
// may call Dispose()" (v0.340):
//
//   "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//    compile error. The borrow receivers: a borrowed local or parameter ..."
//
// ⇒ `il.Dispose()` is the §28.6 compile error and the program must not run.
//
// WHY THE CHECK MISSED IT. `disposeAliasSource` (OwnershipState.hpp) records,
// for a local bound to a bare identifier read of an OWNER, the source of that
// read. The §28.6 receiver check consults the field once, so it recognizes a
// ONE-hop alias (`b`'s recorded source is the owned local `a`) and misses the
// two-hop chain: `b` is itself a borrow, so `il`'s recorded source would be
// `b` — not an owner — and nothing was recorded for `il` at all.
//
// WHY THE DIAGNOSTIC NAMES THE ROOT. The §28.6 wording the check emits is
// "it is a borrow (an alias) of `X`, which still owns it" — and only the ROOT
// of the chain satisfies "which still owns it": `b` does not own the array
// `il` borrows, `a` does. The fix therefore records the chain's ROOT in the
// field, so the message names `a` — the owner — and the diagnostic TEXT is
// unchanged (the one-hop case's message is byte-identical).
//
// ORACLES. Every REJECT row asserts the SPECIFIC diagnostic (the outcome, not
// "did not crash") and runs through `runAotChecked`, so the generated-code
// ASan/LSan tier is armed: on the pre-fix compiler the same program RUNS and
// the sanitizer catches the double free, which is not a compile-time
// rejection, so the row fails on the pre-fix code (fail-without, proved
// mechanically in the commit body). Every ACCEPT-side control row runs under
// `runAotChecked` too, loop-amplified, so a fix that over-rejected, or one
// that left a leak or a second free behind, shows up as a sanitizer report on
// the SAME line.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// =====================================================================
// REJECT arm — §28.6: a `Dispose()` through a BORROW is a compile error,
// however many hops the borrow chain has.
// =====================================================================

// The issue's own repro, verbatim: an IList-typed alias of an ALIAS of the
// owner. The diagnostic names the ROOT owner `a` — not the intermediate
// `b`, which does not own the value.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_interface_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "var il: IList<int> := b; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The CONCRETE-typed spelling of the same two-hop chain: §28.6's rule is
// about the RECEIVER's ownership, not its static type.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_concrete_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "var c: Array<int> := b; "
        "c.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// A THREE-hop chain — the walk is not depth-limited to two.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           three_hop_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "var c: Array<int> := b; "
        "var il: IList<int> := c; "
        "il.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The `?.` spelling — §28.6 names it explicitly ("the `x?.Dispose()` /
// `x!.Dispose()` spellings").
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_nullcond_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "var il: IList<int> := b; "
        "il?.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The `!.` spelling — the same §28.6 sentence, the other named spelling.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_bang_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "var il: IList<int> := b; "
        "il!.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The `const` spelling of the SECOND hop — §21.1 tracks a `const` exactly as
// a plain unmarked `var` alias (fix #4236's send-back established that for
// `disposeAliasSource`).
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_const_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "const c: Array<int> := b; "
        "c.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The REASSIGNMENT spelling of the second hop — the alias is bound after the
// declaration, so the classification is re-derived at the assignment.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_reassigned_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "var c: Array<int>; "
        "c := b; "
        "c.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The `using var` spelling of the second hop — §28.10's borrow-pin case: the
// pin disposes nothing, so an explicit `Dispose()` through it frees the
// owner's array a second time.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_using_var_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var b: Array<int> := a; "
        "using var c: Array<int> := b; "
        "c.Dispose(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// The chain inside a LAMBDA — the closure frame does not change the
// receiver's ownership (§28.3: a captured binding is read as a borrow), so
// the §28.6 verdict stands.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_alias_in_lambda_dispose_rejected_28_6) {
    auto r = runAotChecked(prog(
        "var a: Array<int> := new Array<int>(2); "
        "var f := lambda() => do "
        "  var b: Array<int> := a; "
        "  var il: IList<int> := b; "
        "  il.Dispose(); "
        "end; "
        "f(); return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep compiling
// and running. A false-reject here would be worse than the defect.
// =====================================================================

// (a) The two-hop chain that is NEVER disposed — the check fires at the
//     call site, not at the binding, so a chain of borrows that is only READ
//     stays admitted. Loop-amplified, so a leak or a second free shows up as
//     a sanitizer report on the same line.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_chain_never_disposed_still_runs_28_6) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a: Array<int> := new Array<int>(2); "
        "  var b: Array<int> := a; "
        "  var c: Array<int> := b; "
        "  var n := c.Count; "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (b) The OWNER's own dispose — `a` owns, so `a.Dispose()` is §28.6's "an
//     owned local" receiver: legal, and it must free exactly once
//     (loop-amplified).
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           owner_own_dispose_frees_exactly_once_28_6) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a: Array<int> := new Array<int>(2); "
        "  a.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (c) The alias REBOUND to a fresh owned value — the §28.3 re-derivation
//     (fix #4269) must still clear the alias fact, so the dispose of the
//     REBOUND slot is legal. This is the control that the chain-root
//     recording does not survive a rebind.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           one_hop_alias_rebound_to_fresh_owner_still_disposes_28_6) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a: Array<int> := new Array<int>(2); "
        "  var b: Array<int> := a; "
        "  b := new Array<int>(3); "
        "  b.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (d) The same rebind control, TWO hops deep — the chain-root fact must be
//     re-derived (cleared) at the rebind exactly like the one-hop fact.
BEZEL_TEST(ownership_dispose_alias_chain_7091,
           two_hop_alias_rebound_to_fresh_owner_still_disposes_28_6) {
    auto r = runAotChecked(prog(
        "var i := 0; "
        "while i < 200 do "
        "  var a: Array<int> := new Array<int>(2); "
        "  var b: Array<int> := a; "
        "  var c: Array<int> := b; "
        "  c := new Array<int>(3); "
        "  c.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}
