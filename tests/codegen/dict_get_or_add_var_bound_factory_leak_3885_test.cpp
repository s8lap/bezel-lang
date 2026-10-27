// Codegen — fix #3885 (stdlib §4.8 / §4.9 / language spec §27.5 / §28.9,
// v0.294): `OwningDictionary<K,V>.GetOrAdd(key, factory)` leaked the FACTORY'S
// ENTIRE closure representation -- its own 40-byte cell, its environment
// block, AND the captured variable's own boxed cell -- when the factory
// arrives as a `var`-BOUND REFERENCE (an `ast::IdentifierExpr`) rather than a
// lambda LITERAL at the call site. Reproduces identically before and after
// fix #3871, which admitted only the LITERAL shape.
//
// Root cause. `collectClosureReleaseEligible`'s `ClosureEscapeWalker`
// (LlvmBackend_Setup.cpp) decides whether a `var`-bound closure LOCAL's own
// cell/env gets a scope-end `bz_closure_release`. Its CallExpr arm treated
// EVERY call argument as an unconditional escape ("the callee may store the
// function value") with no exception for a call-bounded builtin position --
// unlike `collectCellFreeEligible`'s own CallExpr arm, which fix #3871 taught
// a GetOrAdd-specific admission for a lambda LITERAL at the factory (second)
// argument position. Because `ast::IdentifierExpr` and `ast::LambdaExpr` are
// SIBLINGS under `ast::Expr`, #3871's `dynamic_cast<const ast::LambdaExpr*>`
// can never match a `var`-bound factory -- so a `var factory := lambda(...)
// => ...; d.GetOrAdd(key, factory);` local was classified an unconditional
// escape by `ClosureEscapeWalker`, never entered `closureReleaseEligible_`,
// and NEITHER its own cell/env (this walker's own scope-end release, gated on
// that set) NOR -- transitively, via `collectCellFreeEligible`'s
// `LambdaFinder`, which classifies a producer lambda's OWN escape status by
// following its owning LocalDecl's `closureReleaseEligible_` membership --
// its captures were ever released: a strictly LARGER leak than #3871's (the
// WHOLE closure, not just the captured box).
//
// The fix: `ClosureEscapeWalker::expr`'s CallExpr arm (LlvmBackend_Setup.cpp)
// gets the analogous admission #3871 gave `collectCellFreeEligible`: a
// bare-identifier read of the tracked closure LOCAL at the factory (second)
// argument position of a `.GetOrAdd(key, factory)` call on a builtin
// Dictionary/OwningDictionary/SortedDictionary receiver (`Emitter::
// isDictTypeName` -- the SAME #1414 registry predicate; no new predicate) is
// a safe (non-escaping) read there, mirroring stdlib §4.8's "GetOrAdd's
// factory is call-bounded" (incorporated by reference into §4.9). Once
// `factory`'s LocalDecl is admitted to `closureReleaseEligible_`, the
// PRE-EXISTING, unmodified machinery does the rest with NO further codegen
// changes: the scope-end `closureRelease` disposal arms directly off that set
// (LlvmBackend_Setup.cpp's `lowerLocalDecl`), and `collectCellFreeEligible`'s
// `LambdaFinder` -- which runs AFTER `collectClosureReleaseEligible` and
// reads its result -- now classifies `factory`'s OWN initializing lambda
// literal as a non-escaping "top-level position" producer exactly as it
// already does for any ordinary (non-GetOrAdd) `var f := lambda(...)`, so the
// captured variable's box is admitted to `cellFreeEligible_` and freed via
// the same `consumeLastLambdaCapturedCells()` call the cell/env release path
// already makes. Unlike #3871, LlvmBackend_Calls.cpp's `GetOrAdd` codegen arm
// needs NO change: it already gates BOTH `consumeLastLambdaCapturedCells()`
// and `emitClosureRelease(closure)` on `factoryIsInlineLambda` (a `var`-bound
// factory is a BORROW there, never released at the call site -- see #422's
// comment in that arm) -- so the call site correctly leaves disposal to the
// factory local's OWN scope end, which this fix is what finally arms.
//
// Guard-sensitivity / no-double-free (per the issue's explicit caution): a
// stored factory can be read at MORE than one GetOrAdd call site, or shared
// between two dictionaries. The THIRD test below passes the SAME `var`-bound
// `factory` to TWO separate `GetOrAdd` calls on TWO separate dictionaries in
// one scope. There is only ONE `ast::LambdaExpr` AST node in this program
// (factory's own initializer) -- `lowerLambda` runs for it exactly ONCE, at
// `factory`'s OWN declaration, regardless of how many downstream reads of the
// identifier there are -- so this fix cannot register more than one
// `closureRelease` / `boxedCellFree` pair for `factory`, however many times
// it is subsequently read as a GetOrAdd factory argument.
//
// Every leak-sanitizer row is loop-amplified (300x, inside the LANE_RULES
// 200-500x band): a one-shot LSan run can pass on BOTH sides of this exact
// fix because a leaked few-byte allocation is a prime candidate to still be
// stack-reachable at process exit on a single invocation.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

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
// stdlib §4.9 / §28.9 -- MISS path, `var`-bound factory: a fresh `using var`
// OwningDictionary each iteration, so the literal key "k" is always absent --
// the factory runs and captures `captured` (a per-iteration local), but is
// bound to a `var` and passed BY REFERENCE, not written inline. FAIL-WITHOUT
// the fix (measured on the unmodified base): LSan reports 52 B / 3 objects
// leaked PER ITERATION (15600 B / 900 allocations at this loop count) -- the
// closure's own cell (40 B) + its 1-slot env block (8 B) + the captured int's
// boxed cell (4 B). PASS-WITH: clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_var_bound_factory_leak_3885,
           miss_path_var_bound_factory_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var i := 0; "
        "while i < 300 do "
        "  var captured := i; "
        "  var factory := lambda(k: string) => move (new R(captured)); "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var v := d.GetOrAdd(\"k\", factory); "
        "  if v.Tag <> captured then return 1; end; "
        "  i := i + 1; "
        "end; "
        "return int(R._n <> 300); "));  // 0 iff every MISS-produced R is disposed
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// stdlib §4.9 / §28.9 -- HIT path, `var`-bound factory: the key is
// pre-populated, so GetOrAdd's own factory is NEVER INVOKED -- but the
// closure `factory` names (and its env, and the captured int's boxed cell)
// still exists and is bound in the caller's own scope, so it must still be
// reclaimed at `factory`'s own scope end. FAIL-WITHOUT the fix (measured):
// identical 52 B / 3 objects leaked PER ITERATION -- the leak is unconditional
// on whether the factory body ever runs, since it is the LOCAL's own
// bind-to-release lifetime that is broken, not anything about invocation.
// PASS-WITH: clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_var_bound_factory_leak_3885,
           hit_path_var_bound_factory_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var i := 0; "
        "while i < 300 do "
        "  var captured := i; "
        "  var factory := lambda(k: string) => move (new R(captured)); "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"k\", move (new R(-1))); "
        "  var v := d.GetOrAdd(\"k\", factory); "
        "  if v.Tag <> -1 then return 1; end; "  // HIT: factory's R(captured) never made
        "  i := i + 1; "
        "end; "
        "return int(R._n <> 300); "));  // 0 iff every pre-populated R is disposed
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// GUARD-SENSITIVITY control (per the issue's explicit caution): the SAME
// `var`-bound `factory` local is passed to TWO SEPARATE `GetOrAdd` calls on
// TWO SEPARATE dictionaries in the same scope. `factory` is declared -- and
// `lowerLambda` runs -- exactly ONCE; both `GetOrAdd` calls merely READ the
// already-existing identifier. This is where an over-free from this fix
// would concretely show up if the widening somehow caused `factory`'s cell,
// env, or captured box to be registered for release more than once (e.g. if
// each "safe read" site independently re-armed a disposal instead of the
// disposal being armed exactly once at `factory`'s OWN declaration).
//
// GUARD-SENSITIVITY (both measured, relinked `bezel_tests_codegen` each
// time):
//   WITHOUT this fix (src/ reverted to the parent commit): FAILS -- `factory`
//     is classified an unconditional escape by the (unfixed) walker, so NONE
//     of its cell/env/captured-box is ever released: LSan reports 15600 B /
//     900 allocations at 300 iterations (52 B/iteration -- the SAME
//     per-iteration signature as the single-call tests above, confirming
//     sharing the factory across two calls does not change the STEADY-STATE
//     leak shape, only that it is now a single closure shared by two reads).
//   WITH this fix (current tree): PASSES -- clean under FULL leak detection
//     (`runAotChecked`), proving both that the leak is closed AND that no
//     double-free is introduced by admitting a closure LOCAL that is read as
//     a GetOrAdd factory more than once.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_var_bound_factory_leak_3885,
           reused_var_bound_factory_two_dicts_no_double_free_27_5) {
    JitResult r = runAotChecked(progR(
        "var i := 0; "
        "while i < 300 do "
        "  var captured := i; "
        "  var factory := lambda(k: string) => move (new R(captured)); "
        "  using var d1 := new OwningDictionary<string, R>(); "
        "  var v1 := d1.GetOrAdd(\"k\", factory); "
        "  using var d2 := new OwningDictionary<string, R>(); "
        "  var v2 := d2.GetOrAdd(\"k\", factory); "
        "  if v1.Tag <> captured or v2.Tag <> captured then return 1; end; "
        "  i := i + 1; "
        "end; "
        // One R stored by EACH of the two dicts per iteration == 600.
        "return int(R._n <> 600); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
