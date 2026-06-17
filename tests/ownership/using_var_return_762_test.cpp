// Spec-derived tests for returning a `using var` (issue #762).
//
// §28.3 (spec line ~4938): "A `using var` is owned but is additionally bound to
// its scope: it cannot be moved out of, only borrowed from. To transfer a
// `using var`'s value out, the programmer must … restructure to introduce the
// value as a non-`using var` owned local."
// §28.3 (line ~4988) names the diagnostic: "Cannot move out of `using var` `x`;
// `using var` is scope-bound."
//
// Returning a `using var` by bare identifier transfers it OUT of its scope:
//   - under an `owned` return type it is a move-on-read into the owned return
//     slot (a move out of the scope-bound local), and
//   - under a borrow return type it is a borrow that escapes the scope that
//     disposes the value (a dangling borrow).
// Either way the value would be disposed both by the receiver and by the
// using-var's scope-bound discharge — a double-free / use-after-free.
//
// The under-enforcement this file pins down (and #762 fixes): the bare-
// identifier return fast-path in `handleReturn` marked the local discharged and
// returned before BOTH the owned-move-out and borrow-escape checks, so the
// return was silently accepted. It is now rejected, mirroring the
// `markMovedByName` scope-bound guard.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool rejectsUsingVarReturn(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`using var`") != std::string::npos &&
            d.message.find("scope-bound") != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — returning a `using var` is rejected (§28.3).
// =============================================================================

// §28.3: `return w` of a `using var` under an `owned` return type is a move-out
// of a scope-bound local — REJECTED. This is the #762 repro.
BEZEL_TEST(using_var_return_762, using_var_owned_return_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget destructor end destructor; "
        "  public static method Bad(): owned Widget "
        "    using var w := new Widget(); "
        "    return w; "
        "  end Bad; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsUsingVarReturn(r));
}

// §28.3: `return w` of a `using var` under a borrow return type is a borrow
// that escapes the scope that disposes it — REJECTED.
BEZEL_TEST(using_var_return_762, using_var_borrow_return_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget destructor end destructor; end Widget; "
        "class C "
        "  public static method BadBorrow(): Widget "
        "    using var w := new Widget(); "
        "    return w; "
        "  end BadBorrow; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsUsingVarReturn(r));
}

// =============================================================================
// POSITIVE — legal owned-local transfers are unaffected.
// =============================================================================

// §28.3: a plain (non-`using`) owned local is the transfer form the spec points
// to ("restructure to introduce the value as a non-`using var` owned local") —
// `return w` moves it out to the caller. ACCEPTED.
BEZEL_TEST(using_var_return_762, plain_owned_local_return_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget destructor end destructor; "
        "  public static method Ok(): owned Widget "
        "    var w := new Widget(); "
        "    return w; "
        "  end Ok; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_FALSE(rejectsUsingVarReturn(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3: the rejection keys on the RETURNED identifier, not on the mere presence
// of a `using var` in scope — returning a plain owned local while a `using var`
// guard is live is fine (the guard discharges at scope end). ACCEPTED.
BEZEL_TEST(using_var_return_762, return_plain_local_with_using_var_in_scope_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget destructor end destructor; "
        "  public static method Ok(): owned Widget "
        "    using var guard := new Widget(); "
        "    var result := new Widget(); "
        "    return result; "
        "  end Ok; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_FALSE(rejectsUsingVarReturn(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.2: a `using var` that is simply used and discharged at scope end (no
// return) is the normal, legal pattern — no diagnostic. ACCEPTED.
BEZEL_TEST(using_var_return_762, using_var_scope_discharge_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class Widget public n: int := 0; destructor end destructor; "
        "  public static method Use(): int "
        "    using var w := new Widget(); "
        "    return w.n; "
        "  end Use; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_FALSE(rejectsUsingVarReturn(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
