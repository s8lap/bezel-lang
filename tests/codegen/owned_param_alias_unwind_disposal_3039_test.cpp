// codegen/ownership — fix #3039 (§28.3 / §28.10, v0.240): the EXCEPTION-PATH
// residual of #3035. The W7L2 #3035 fix shipped the normal-exit half: a
// plain `var alias := r;` over an `owned` parameter `r` no longer disables
// the auto-drop at method end (the auto-drop bail in `registerUnwindOwnedParam`
// was moved below the `autoDropAtScopeEnd` branch). But the EXCEPTION-EDGE
// disposal still bailed for the same alias binding at line 15254 — the
// `unwindOnly` branch's residual `unwindIneligible_` bail was untouched.
// The walker (UnwindIneligibleWalker at src/codegen/LlvmBackend_Setup.cpp,
// `stmt(LocalDecl)`) over-approximated the unmarked alias binding as a
// CONSUME (consuming=true in the catch-all expr() call), so the walker
// tainted `r` as unwind-ineligible; the bail then dropped the registration
// on the exception edge. A method that takes an `owned r: T`, has a plain
// `var alias := r;` (a K3.13 borrow), and exits via a `throw` between the
// alias and method end, leaks `r` — `R._n == 0` and LSan flags a 16-byte
// leak from `main` (the `r` parameter's allocation).
//
// SPEC AUTHORITY:
//   - §28.3 K3.13 (v0.240, line 6273): "An **unmarked read of a class-typed
//     binding never consumes**: `var b := a;` is always a borrow for
//     class-typed `a`." — the alias binding does NOT consume `r`.
//   - §28.3 K3.14 (line 6274): an undischarged `owned` parameter is disposed
//     at scope end implicitly (auto-drop on the NORMAL exit — the W7L2
//     half).
//   - §28.3 K3.15 (line 6275): "The exception edge performs the same
//     implicit disposal as the normal edge, on the single establishment-
//     ordered cleanup stack; the only difference is the lint." — K3.15
//     EXCEPTION-EDGE PARITY: a never-discharged owned parameter MUST be
//     disposed on the unwind edge. This test covers K3.15.
//
// WHAT THIS TEST VERIFIES (mirrors the W7L2 #3035 test, but on the
// exception edge):
//
//   1. plain `var alias := r;` + a later `throw` disposes `r` once on the
//      unwind edge (the bug — the residual of #3035).
//   2. plain `var alias := r;` + NORMAL `return` still auto-drops `r` (the
//      W7L2 #3035 fix; included to guard against the new walker branch
//      accidentally breaking the normal path).
//   3. ALIAS-OF-OWNED-LOCAL variant: `var y := r; var alias := y; throw …`
//      — K3.13's "an unmarked read of a class-typed binding" carries
//      through the chain; the parameter still disposes on unwind.
//   4. discard-read does not consume (K3.13 second shape): a plain
//      `var alias := r;` + `_ := alias;` + `throw` — the discard read is
//      a borrow, the throw edge still disposes.
//
// FAIL-WITHOUT / PASS-WITH: before the fix the destructor count is 0 in
// cases 1, 3 (throw-edge cells), and 4 (the alias chain still taints via
// the catch-all walker); case 2 (normal-exit control) stays clean (the
// W7L2 #3035 fix covers it). Case 5 is a no-op guard. The LSan leg of
// runAotChecked also flips `ran` to false on any leak, so a leak fails
// both the count assertion AND the LSan check.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Disposed-exactly-once oracle: a leak / double-free fails the checked run
// (ran=false -> -999), so the sanitizer verdict rides on the same assertion
// as the destructor-count exit code.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            std::cout << "  diag: " << d.message << "\n";
        }
        return -999;
    }
    return r.exitCode;
}

// Resource class R: destructor bumps a static disposal counter R._n.
// `V` is an instance field so `alias.V` (a borrow read) accesses an
// instance member, not the static counter.
const char* kR =
    "class R "
    "  public V: int := 0; "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; ";

// Throwable marker class E for the exception-path case.
const char* kE = "class E: Exception end E; ";

std::string prog(const std::string& classesAfterR, const std::string& mainBody) {
    return std::string("module M; ") + kR + classesAfterR +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// Case 1 — fail-without proof (#3039 headline): a plain `var alias := r;`
// over an `owned` parameter `r`, with a later `throw` between the alias
// and method end, must dispose `r` once on the unwind edge. Before the fix
// the `unwindIneligible_` bail in `registerUnwindOwnedParam`'s
// `unwindOnly` branch (line 15254) dropped the registration for a
// parameter the walker had tainted at the alias-binding site; the
// exception edge's implicit try/finally therefore ran no disposal and `r`
// leaked. Measured: `R._n == 0` + a 16-byte LSan leak from `main` (the
// `r` parameter's allocation). Failing the count AND the LSan verdict
// fails both legs of runAotChecked. The fix adds the K3.13-positive
// recognition in `UnwindIneligibleWalker::stmt(LocalDecl)` so the source
// of a plain `var alias := <owned param>;` walks as a borrow (not as a
// consume), the bail no longer fires for this shape, and the disposal
// runs.
// ===========================================================================
BEZEL_TEST(owned_param_alias_unwind_disposal_3039,
           plain_var_alias_then_throw_disposes_on_unwind_28_3_K3_15) {
    const std::string src = prog(
        "class Q "
        "  public static method Throw(r: owned R) "
        "    var alias := r; "        // K3.13 borrow
        "    _ := alias; "            // borrow read
        "    throw new E(); "          // K3.15 unwind edge
        "  end Throw; "
        "end Q; " + std::string(kE),
        "  try Q.Throw(new R()); "
        "  catch E => return R._n; end; "  // 1: parameter auto-disposed on unwind
        "  return -1; ");
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 2 — NORMAL-EXIT control (regression guard for the W7L2 #3035 fix):
// a plain `var alias := r;` with the method returning normally still
// auto-drops `r` once at method end. The new walker branch (the K3.13
// positive recognition) MUST keep the normal-exit path clean — the
// `autoDropAtScopeEnd` branch in `registerUnwindOwnedParam` still fires
// for the parameter regardless of the walker verdict, and the dynamic
// dispose is null-guarded. `R._n == 1`.
// ===========================================================================
BEZEL_TEST(owned_param_alias_unwind_disposal_3039,
           plain_var_alias_normal_return_still_auto_drops_28_3_K3_14) {
    const std::string src = prog(
        "class Q "
        "  public static method Consume(r: owned R): int "
        "    var alias := r; "
        "    return alias.V; "        // alias.V is borrowed here (K3.13)
        "  end Consume; "
        "end Q; ",
        "  do Q.Consume(new R()); end; "
        "  return R._n; ");           // 1: callee auto-dropped the owned param once
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 3 — alias-of-alias (K3.13 chain): `var y := r; var alias := y;`
// the second alias's source is `y` (a plain `var` owned local), the
// first alias's source is `r` (an `owned` parameter). The walker's new
// K3.13 branch accepts BOTH (the inner `y` is a plain `var` owned local
// at scope entry, the outer `r` is an `owned` parameter). On the throw
// edge, both alias bindings are walks-as-borrow; the parameter's slot
// stays eligible and the unwind disposal fires once. `R._n == 1`.
// Regression guard against the new branch's predicate accidentally
// rejecting a chained read.
// ===========================================================================
BEZEL_TEST(owned_param_alias_unwind_disposal_3039,
           alias_of_owned_local_then_throw_param_still_disposes_28_3_K3_13) {
    const std::string src = prog(
        "class Q "
        "  public static method Chained(r: owned R) "
        "    var y := r; "
        "    var alias := y; "
        "    _ := alias; "
        "    throw new E(); "
        "  end Chained; "
        "end Q; " + std::string(kE),
        "  try Q.Chained(new R()); "
        "  catch E => return R._n; end; "  // 1: K3.13 chain stays borrow
        "  return -1; ");
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 4 — discard-read does not consume (K3.13 second shape): a plain
// `var alias := r;` followed by `_ := alias;` and a `throw` — the discard
// read is a BORROW (it does not move `alias`), so `r` still owns its
// obligation through the alias; on the unwind edge the auto-dispose
// fires (K3.15). This is the SAME shape as Case 1 but with a discard
// read of the alias in addition — guards against a regression that
// might treat the `_ := alias` read as a consume of `alias` (it is
// not; a discard is a borrow per §28 line ~4899) and thus as a consume
// of `r` through the alias. `R._n == 1`.
// ===========================================================================
BEZEL_TEST(owned_param_alias_unwind_disposal_3039,
           discard_alias_read_then_throw_param_disposes_28_3_K3_15) {
    const std::string src = prog(
        "class Q "
        "  public static method DiscardThrow(r: owned R) "
        "    var alias := r; "
        "    _ := alias; "                  // discard = borrow read
        "    throw new E(); "
        "  end DiscardThrow; "
        "end Q; " + std::string(kE),
        "  try Q.DiscardThrow(new R()); "
        "  catch E => return R._n; end; "   // 1: discard doesn't consume
        "  return -1; ");
    BEZEL_EXPECT_EQ(run(src), 1);
}