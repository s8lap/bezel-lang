// Codegen — fix #3871 (stdlib §4.8 / §4.9 / language spec §27.5 / §28.9,
// v0.294): `OwningDictionary<K,V>.GetOrAdd(key, factory)` leaked the factory
// closure's ENVIRONMENT — specifically, the boxed heap cell of a variable the
// factory lambda CAPTURES from its enclosing scope — independent of whether
// the call took the hit or the miss path, and independent of the key's shape
// (a plain literal key reproduces it; this was never about the key).
//
// Authority. Stdlib §4.8 states outright (in its entry-shaped-collection
// paragraph) "GetOrAdd's factory is call-bounded", incorporated by reference
// into §4.9's OwningDictionary. Language spec §28.9 ("Runtime-managed types")
// is the closure-lifetime discipline that "call-bounded" invokes: a
// call-bounded factory cannot outlive the call that received it, so a local
// variable captured ONLY by such a factory is safe to reclaim at the
// variable's own scope end — exactly the existing "IIFE" and "§28.14 scoped
// register position" admissions in `collectCellFreeEligible`
// (LlvmBackend_Setup.cpp) already give an assigned-to-`var` or IIFE lambda,
// and the #3498 fix gives a lambda argument to a locally-compiled user
// method whose sink was proven call-bounded by
// `MethodOwnership::callBoundedScan`.
//
// Root cause. GetOrAdd is a BUILTIN (LlvmBackend_Calls.cpp's `m ==
// "GetOrAdd"` arm, no `ast::MethodDecl` body), so #3498's
// `isCallBoundedClosureArgPosition` — which requires a locally-compiled user
// method to have actually run `callBoundedScan` against — never admits it.
// The pre-scan therefore fell through to the generic "any lambda not
// directly bound to a non-escaping `var`" default and classified the factory
// ESCAPING, so `cellFreeEligible_` never included the captured local and its
// boxed cell was never freed — even though GetOrAdd's own codegen invokes
// the factory synchronously on the miss path only and unconditionally
// `emitClosureRelease`s the closure at the join point on BOTH the hit and
// miss paths, so it provably never outlives the call.
//
// The fix (two halves, mirroring #2182's IIFE fix and #2334's TaskScope.Spawn
// fix exactly):
//   1. `collectCellFreeEligible`'s `LambdaFinder::expr` CallExpr arm
//      (LlvmBackend_Setup.cpp) gets a GetOrAdd-specific admission: a lambda
//      LITERAL in the factory (second) argument position of a
//      `.GetOrAdd(key, factory)` call whose receiver is a builtin
//      Dictionary/OwningDictionary/SortedDictionary (`isDictTypeName` — the
//      SAME #1414 stage-3 registry predicate LlvmBackend_Calls.cpp's own
//      `isDict` gate for this exact arm uses) is admitted non-escaping.
//   2. LlvmBackend_Calls.cpp's `m == "GetOrAdd"` arm calls
//      `consumeLastLambdaCapturedCells()` right after lowering the factory
//      closure (gated on the argument actually being a lambda literal,
//      exactly like the pre-existing `factoryIsInlineLambda` gate the arm
//      already computes for `emitClosureRelease`) — without this half the
//      pre-scan's admission is inert (#2182's own comment: "Admitting here is
//      only HALF of what the shape needs").
//
// Both fixes are narrowly scoped to the GetOrAdd factory argument: a factory
// value arriving via a variable/field (not a lambda literal at the call
// site) is an `ast::IdentifierExpr`, a SIBLING of `ast::LambdaExpr` under
// `ast::Expr` — `dynamic_cast<const ast::LambdaExpr*>` can never succeed on
// one, by construction of the type hierarchy, independent of anything else
// in this fix. That is a type-system fact, not something a runtime test can
// stress, so no test asserts it directly; a `var`-bound factory passed to
// GetOrAdd is a separate, unrelated pre-existing defect (leaks the whole
// closure, not just the captured box) filed as #3885.
//
// The genuinely untested risk for a LITERAL factory is a captured local
// reachable from the admission TWICE — two separate `GetOrAdd` calls in the
// same scope, each with its own literal factory, both capturing the SAME
// outer local. Naively, `lowerLambda` could push the same cell into
// `lastLambdaCapturedCells_` a second time and `consumeLastLambdaCapturedCells()`
// could then register a SECOND `boxedCellFree` for the same slot at scope
// end — a double-free, not a leak. `two_getoradd_calls_share_one_captured_
// local_no_double_free_27_5` below exercises exactly this and is proven
// guard-sensitive (§below): the codebase's pre-existing `!alreadyBoxed` gate
// (`boxLocalSlot`, `LlvmBackend_Exprs2.cpp:~1310`) already prevents the
// second `lowerLambda` call from re-adding the cell, so this fix does not
// introduce a double-free for this shape — but nothing exercised that
// combination before this test.
//
// Every leak-sanitizer row is loop-amplified (300x, inside the LANE_RULES
// 200-500x band): a one-shot LSan run can pass on BOTH sides of this exact
// fix because a leaked 4-byte allocation (one captured `int`'s box) is a
// prime candidate to still be stack-reachable at process exit on a single
// invocation.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent — mirrors dict_string_key_leak_3703_test.cpp / dict_get_or_add_test.cpp.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// A resource class: a static counter tracks live-vs-disposed instances and a
// Tag field carries a per-instance value, set from the factory's capture.
std::string progR(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.9 / §28.9 — MISS path: a fresh `using var` OwningDictionary each
// iteration, so the literal key "k" is always absent — the factory runs and
// captures `captured` (a per-iteration local). FAIL-WITHOUT the fix (measured
// on the unmodified base): LSan reports 4 B / 1 object leaked PER ITERATION
// (1200 B / 300 allocations at this loop count) — the captured int's boxed
// cell. PASS-WITH: clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_capturing_factory_leak_3871,
           miss_path_capturing_factory_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var i := 0; "
        "while i < 300 do "
        "  var captured := i; "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var v := d.GetOrAdd(\"k\", lambda(k: string) => move (new R(captured))); "
        "  if v.Tag <> captured then return 1; end; "
        "  i := i + 1; "
        "end; "
        "return int(R._n <> 300); "));  // 0 iff every MISS-produced R is disposed
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// stdlib §4.9 / §28.9 — HIT path: the key is pre-populated, so GetOrAdd's own
// factory is NEVER INVOKED — but the closure (and its env, and the captured
// int's boxed cell) is still CONSTRUCTED before the hit/miss probe runs
// (GetOrAdd lowers the factory closure once, up front) and must still be
// reclaimed. FAIL-WITHOUT the fix (measured): identical 4 B / 1 object
// leaked PER ITERATION — the leak fires on the hit path exactly as on the
// miss path, since it is the closure's ENV that leaks, not anything to do
// with whether the factory body ever runs. PASS-WITH: clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_capturing_factory_leak_3871,
           hit_path_capturing_factory_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var i := 0; "
        "while i < 300 do "
        "  var captured := i; "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"k\", move (new R(-1))); "
        "  var v := d.GetOrAdd(\"k\", lambda(k: string) => move (new R(captured))); "
        "  if v.Tag <> -1 then return 1; end; "  // HIT: factory's R(captured) never made
        "  i := i + 1; "
        "end; "
        "return int(R._n <> 300); "));  // 0 iff every pre-populated R is disposed
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// GUARD-SENSITIVITY control: TWO SEPARATE `GetOrAdd` calls in the SAME scope,
// EACH with its OWN lambda LITERAL factory, both capturing the SAME outer
// `captured` local. Both factory arguments ARE `ast::LambdaExpr` literals —
// this program DOES reach both halves of the fix (twice), unlike an
// identifier-based control, which can never reach either half by construction
// of the AST type hierarchy (`IdentifierExpr` and `LambdaExpr` are siblings
// under `ast::Expr`).
//
// This is where an over-free from this fix would concretely show up: if
// `lowerLambda`'s SECOND call (for the second literal factory) re-pushed
// `captured`'s cell into `lastLambdaCapturedCells_`, then
// `consumeLastLambdaCapturedCells()` would register TWO `boxedCellFree`
// disposals for the SAME slot in the SAME scope — a double-free at scope end,
// not a leak. It does not: `boxLocalSlot`'s pre-existing `!alreadyBoxed` gate
// (LlvmBackend_Exprs2.cpp:~1310) means the second capture site sees the local
// already boxed and never re-adds it, so only ONE `boxedCellFree` is ever
// registered per captured local per scope, regardless of how many literal
// factories capture it. This test is the first to exercise that combination
// against GetOrAdd specifically.
//
// GUARD-SENSITIVITY (both measured, relinked `bezel_tests_codegen` each time):
//   WITHOUT this fix (both src/ hunks reverted): FAILS — neither GetOrAdd
//     call's capture is ever admitted to `cellFreeEligible_`, so `captured`'s
//     box is never freed at all: LSan reports 1200 B / 300 allocations (one
//     box per iteration — confirming the leak count is NOT doubled by the two
//     capture sites either, i.e. the dedup guard is orthogonal to this fix).
//   WITH this fix (current tree): PASSES — clean under FULL leak detection
//     (`runAotChecked`, not the lenient oracle: this program uses two LITERAL
//     factories, so it never reaches #3885's separate var-bound-factory
//     defect, unlike the identifier-based shape this control replaced).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_capturing_factory_leak_3871,
           two_getoradd_calls_share_one_captured_local_no_double_free_27_5) {
    JitResult r = runAotChecked(progR(
        "var i := 0; "
        "while i < 300 do "
        "  var captured := i; "
        "  using var d1 := new OwningDictionary<string, R>(); "
        "  var v1 := d1.GetOrAdd(\"k\", lambda(k: string) => move (new R(captured))); "
        "  using var d2 := new OwningDictionary<string, R>(); "
        "  var v2 := d2.GetOrAdd(\"k\", lambda(k: string) => move (new R(captured))); "
        "  if v1.Tag <> captured or v2.Tag <> captured then return 1; end; "
        "  i := i + 1; "
        "end; "
        // One R stored by EACH of the two dicts per iteration == 600.
        "return int(R._n <> 600); "));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
