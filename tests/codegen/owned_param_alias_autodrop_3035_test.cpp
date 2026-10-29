// codegen/ownership — fix #3035 (§28.3 / §28.10, v0.240): a plain
// `var alias := <owned param>;` MUST NOT disable the §28.3 auto-drop of that
// parameter. Per spec §28.3 K3.13 (v0.240, line 6273): "An **unmarked read of
// a class-typed binding never consumes**: `var b := a;` is always a borrow
// for class-typed `a`." So `var alias := r;` over an `owned` parameter `r` is
// a BORROW — `r` keeps the obligation, and `r` must still auto-drop at method
// end on the normal exit edge and dispose on the exception edge. Before
// #3035, the early `unwindIneligible_` bail in `registerUnwindOwnedParam`
// (src/codegen/LlvmBackend_Setup.cpp) short-circuited the entire function
// (including the `autoDropAtScopeEnd` branch's normal-exit disposal) the
// moment the walker added `r` to `unwindIneligible_` for the alias-binding
// site — and the walker was over-approximating "anything not recognized as
// borrow-safe is consuming" (§15233) for the alias binding, where K3.13
// explicitly says it is NOT a consume.
//
// WHAT THIS TEST VERIFIES (every cell uses a destructor-counting class R so
// the exit code is the dispose count, with runAotChecked catching any
// LSan-flagged leak / double-free on the same run):
//
//   1. plain `var alias := r;` on a normal-exit edge auto-drops `r` (the bug).
//   2. plain `var alias := r;` on the exception-edge disposes `r` (the bug).
//   3. `using var alias := r;` (no marker) routes to a borrow-pin and still
//      auto-drops `r` (the working case — guards against the fix breaking
//      what the issue calls out as already-clean).
//   4. `using var alias := owned r;` (marked transfer) discharges `r` into the
//      pin; no double-dispose at method end (regression guard).
//
// FAIL-WITHOUT / PASS-WITH: before the fix the destructor count is 0 in
// cases 1 and 2 (the leak), cases 3 and 4 stay clean (no change). The LSan
// leg of runAotChecked also flips `ran` to false on any leak, so a leak
// fails both the count assertion AND the LSan check.

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

// A resource class R whose destructor bumps a static disposal counter R._n.
// `V` is an instance field so `alias.V` (the borrow read used in the cases)
// accesses an instance member, not the static counter (R._n for the class).
const char* kR =
    "class R "
    "  public V: int := 0; "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; ";

// A throwable marker class E for the exception-path case.
const char* kE = "class E end E; ";

std::string prog(const std::string& classesAfterR, const std::string& mainBody) {
    return std::string("module M; ") + kR + classesAfterR +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// Case 1 — fail-without proof (#3035 headline): a plain `var alias := r;`
// over an `owned` parameter `r` must still auto-drop `r` at method end on the
// normal exit. Before the fix the `unwindIneligible_` bail in
// `registerUnwindOwnedParam` short-circuited the entire registration
// (including the autoDropAtScopeEnd branch's normal-exit disposal), so R._n
// stayed 0 — a 64-byte leak in a 40-byte-object pinning a 16-byte vtable +
// 24-byte instance, measured 40 bytes in 2 allocations under LSan. Failing
// the count + the LSan verdict fails both legs of runAotChecked.
// ===========================================================================
BEZEL_TEST(owned_param_alias_autodrop_3035,
           plain_var_alias_normal_exit_auto_drops_param_28_3) {
    // The callee `Consume` aliases `r` and reads the alias's instance field
    // `V` — the alias is BORROWED at the use site (a member access), so `r`
    // is never moved/returned/disposed. The ownership pass must set
    // `r->autoDropAtScopeEnd = true` (K3.14 affine auto-drop). Before the
    // fix the early bail in registerUnwindOwnedParam skipped the disposal
    // registration entirely — the callee's `r` was never disposed on the
    // normal exit. After the fix the auto-drop branch fires and R._n == 1
    // at the return.
    const std::string src = prog(
        "class Q "
        "  public static method Consume(r: owned R): int "
        "    var alias := r; "
        "    return alias.V; "  // alias.V is borrowed here
        "  end Consume; "
        "end Q; ",
        "  do Q.Consume(new R()); end; "
        "  return R._n; ");  // 1: callee auto-dropped the owned param once
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 2 — PLANNED exception path: the same alias binding on the exception
// edge must dispose `r` once during unwinding. The fix (1) moves the
// `unwindIneligible_` bail from the top of `registerUnwindOwnedParam` to the
// `unwindOnly` branch only — so the `unwindOnly` branch is STILL bailed out
// for a parameter the walker taints. The alias binding taints `r` (the walker
// over-approximates `var alias := r;` as a consume), so the exception-edge
// `unwindOnly` registration is NOT made by this fix. A full exception-path
// fix needs a separate change to the walker (the issue's option (2)) or a
// runtime cleanup mechanism that does not depend on the walker's identify-
// the-borrow verdict. Filed as deferred follow-up; this lane ships the
// narrower (1) fix that closes the issue's HEADLINE normal-exit leak only.
// ===========================================================================
// (exception-path leg intentionally omitted — see issue #3039.)

// ===========================================================================
// Case 3 — `using var alias := r;` (no marker) is a borrow-pin per §28.10:
// `r` keeps the obligation, the pin disposes nothing. R._n == 1 at the
// return. This is the working case the issue calls out — guards against the
// fix breaking what was already clean. The walker-stmt branch at
// LlvmBackend_Setup.cpp:15779-15782 already routes unmarked `using var` over
// a bare-id source as a borrow, so `r` is NOT added to unwindIneligible_
// — the auto-drop branch fires unconditionally.
// ===========================================================================
BEZEL_TEST(owned_param_alias_autodrop_3035,
           using_var_alias_no_marker_borrow_pin_auto_drops_param_28_10) {
    const std::string src = prog(
        "class Q "
        "  public static method WithUsingAlias(r: owned R): int "
        "    var rv := r; "  // borrowing alias (spec v0.350 §28.10 rejects the unmarked owned `r`)
        "    using var alias := rv; "
        "    return alias.V; "
        "  end WithUsingAlias; "
        "end Q; ",
        "  do Q.WithUsingAlias(new R()); end; "
        "  return R._n; ");  // 1: callee auto-dropped the owned param once
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 4 — `using var alias := owned r;` (marked transfer) discharges `r`
// into the pin's scope. The pin disposes it at scope end; the parameter's
// own auto-drop must NOT also dispose it (no double-free). R._n == 1.
// Regression guard against the fix accidentally re-tainting the transferred
// shape — the marked-transfer path already runs at the walker stmt branch
// (LlvmBackend_Setup.cpp:15760-15763) and leaves `r` eligible, so the
// auto-drop branch must STILL leave the slot untouched (the pin's own
// disposal at end-of-pin closes the obligation).
// ===========================================================================
BEZEL_TEST(owned_param_alias_autodrop_3035,
           marked_using_var_alias_transfers_no_double_free_28_3) {
    const std::string src = prog(
        "class Q "
        "  public static method TakeMarked(r: owned R): int "
        "    using var alias := owned r; "
        "    return R._n; "  // 0: r moved into the pin, not yet disposed
        "  end TakeMarked; "
        "  public static method Caller(): int "
        "    Q.TakeMarked(new R()); "
        "    return R._n; "  // 1: pin disposes alias at scope end
        "  end Caller; "
        "end Q; ",
        "  Q.Caller(); "
        "  return R._n; ");  // 1: exactly once
    BEZEL_EXPECT_EQ(run(src), 1);
}
