// codegen/ownership — fix #3253 (§28.10 / §28.3 K3.13, v0.256): the
// `using var alias := <owned parameter>;` + discard / throw UAF that is
// the residual of #3039's K3.13 walker untaint.
//
// Spec chain:
//   - §28.10 (v0.256): a `using var` whose RHS is a borrow-pin (an unmarked
//     read of a named owned binding — a plain `var` owned local or an
//     `owned` parameter) does NOT assume a disposal obligation; the source's
//     owner keeps the obligation and the using-var disposes nothing. The
//     slot is the borrow-pin (line 7261-7263).
//   - §28.3 K3.13 (v0.240 line 6273): "An unmarked read of a class-typed
//     binding never consumes: `var b := a;` is always a borrow for class-
//     typed `a`."
//   - §28.3 K3.14 / K3.15: the parameter's auto-drop fires on the normal
//     exit / unwind edge.
//
// What this test pins:
//
//   The OWNERSHIP pass records the `using var` binding as `Owned` +
//   `implicitDischarge` for the AXIOM-A5 immovability + scope-bound machinery
//   (#2072 funnel, Ownership.cpp:4810) but lays the §28.10 borrow-pin SHAPE
//   on a SEPARATE flag (`usingVarSourceIsBorrow`). The pre-fix discard path
//   (Ownership.cpp:2699, `discardOutOwnedVar = true` when the binding is
//   lookupState-Owned) treated the borrow-pin alias as a named owned binding
//   and emitted an EXPLICIT Dispose on its slot via
//   `explicitDisposeOwnedReceiverSlot` (LlvmBackend_Stmts.cpp:2768). The
//   parameter's K3.14 auto-drop ALSO fired on the same slot at scope end —
//   same allocation, same Dispose call, double-dispose (heap-use-after-free /
//   segfault on a `throw` between the alias and method end — the K3.15
//   unwind edge).
//
//   Three shapes pinned here:
//     1. Headline (the bug): `using var alias := r;` + a `throw` before the
//        method end. ASan oracle detects the heap-use-after-free.
//     2. Normal-exit control: `using var alias := r;` + a `_ := alias;`
//        discard read + `return 0`. The discard MUST NOT consume `alias`
//        (a §28.10 borrow-pin has no obligation to consume); exactly-once
//        disposal on the parameter's auto-drop.
//     3. Adoption regression guard: `using var y := owned r;` (MARKED
//        adoption) + `_ := y;` keeps the announced-consume path — the
//        marked adoption genuinely owns the obligation and the discard
//        discharges it.
//
// FAIL-WITHOUT / PASS-WITH: pre-fix the destructor runs TWICE
// (parameter's auto-drop + discard's explicit-dispose) or the ASan heap-
// use-after-free fires; the count assertion + the LSan/ASan verdict both
// fail. Post-fix the destructor runs ONCE.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;

namespace {

// Disposed-exactly-once oracle: a leak / double-free fails the checked run
// (ran=false -> -999), so the sanitizer verdict rides on the same assertion
// as the destructor-count exit code.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            std::cout << "  diag: " << d.message << "\n";
        }
        return -999;
    }
    return r.exitCode;
}

// Resource class R: destructor bumps a static disposal counter R._n. `V` is
// an instance field so `r._n` accesses the static counter (a static field
// read is a borrow, not a consume, per §28.3 K3.13).
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
// Case 1 — fail-without proof (#3253 headline): a `using var alias := r;`
// over an `owned` parameter `r`, with a later `throw` between the alias
// and method end. Pre-fix the OWNERSHIP pass recorded `alias` as
// `Owned + implicitDischarge` and the discard path's announced-consume
// disposed `alias`'s slot; the parameter's K3.15 auto-drop then disposed
// the same slot — heap-use-after-free / segfault on the throw edge
// (same shape as #3039's UAF, on the exception edge rather than the
// plain auto-drop). The fix generalizes the K3.13 borrow-pin carve-out
// to `using var alias := <owned param>;` so `usingVarSourceIsBorrow` is
// set, the using-var's own Dispose is skipped (LlvmBackend_Setup.cpp:
// 13552), AND the discard skips its announced-consume path on the
// aliasing pin (Ownership.cpp fix). Exactly-once disposal, no UAF.
// ===========================================================================
BEZEL_TEST(using_var_alias_owned_param_uaf_3253,
           using_var_alias_then_throw_disposes_exactly_once_28_10_K3_15) {
    const std::string src = prog(
        "class Q "
        "  public static method Throw(r: owned R) "
        "    using var alias := r; "        // §28.10 borrow-pin
        "    _ := alias; "                  // discard — must NOT consume alias
        "    throw new E(); "               // K3.15 unwind edge
        "  end Throw; "
        "end Q; " + std::string(kE),
        "  try Q.Throw(new R()); "
        "  catch E => return R._n; end; "   // 1: parameter auto-disposed once on unwind
        "  return -1; ");
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 2 — NORMAL-EXIT control (regression guard): `using var alias := r;`
// + `_ := alias;` + `return 0` (no throw). Pre-fix the discard emitted
// an explicit Dispose on `alias`'s slot, the parameter's K3.14 auto-drop
// fired on the same slot at method end — double-dispose on the normal
// exit path. Post-fix the discard skips its announced-consume path for
// borrow-pins; the parameter's single auto-drop is exactly-once.
// ===========================================================================
BEZEL_TEST(using_var_alias_owned_param_uaf_3253,
           using_var_alias_then_discard_then_return_auto_drops_once_28_3_K3_14) {
    const std::string src = prog(
        "class Q "
        "  public static method Consume(r: owned R): int "
        "    using var alias := r; "       // §28.10 borrow-pin
        "    _ := alias; "                  // discard — must NOT consume alias
        "    return 0; "
        "  end Consume; "
        "end Q; ",
        "  do Q.Consume(new R()); end; "
        "  return R._n; "                  // 1: parameter auto-dropped once
    );
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 3 — ADOPTION regression guard: `using var y := owned r;` (MARKED
// transfer per §28.10, #1962) + `_ := y;` keeps the announced-consume
// path. The marked adoption genuinely owns the obligation; the discard
// discharges it. `R._n == 1` (the discard's Dispose disposes y once; the
// using-var's scope-end Dispose fires on the now-nulled slot — no-op).
// ===========================================================================
BEZEL_TEST(using_var_alias_owned_param_uaf_3253,
           marked_using_var_adoption_discard_still_discharges_28_10) {
    const std::string src = prog(
        "class Q "
        "  public static method Take(r: owned R): int "
        "    using var y := owned r; "     // MARKED — §28.10 adoption
        "    _ := y; "                      // discard — DOES consume y
        "    return 0; "
        "  end Take; "
        "end Q; ",
        "  do Q.Take(new R()); end; "
        "  return R._n; "                  // 1: y disposed once, scope-end no-op
    );
    BEZEL_EXPECT_EQ(run(src), 1);
}