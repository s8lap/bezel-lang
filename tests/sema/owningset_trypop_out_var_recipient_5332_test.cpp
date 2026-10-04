// Spec-derived tests for fix #5332 — §22.4 "out var declarations" (v0.313).
//
// The BUILTIN borrowing-extraction derivation for a `Try*` callee with no
// MethodDecl carries an arm per builtin collection.  It had Stack / Queue /
// OwningStack / OwningQueue / Channel / Dictionary / OwningDictionary arms but
// NO OwningSet arm.  stdlib §4.10 declares `OwningSet<T>.TryPop(query: T,
// out owned removed: T?): bool`, and stdlib §1 shape (a) puts it in the
// "bool + nullable out" table.
//
// Spec §22.4 (v0.313): "`out var name` declares a new variable inline. Its
// type is inferred from the method's `out` parameter declaration."  The
// `out using var` form (§22.4 / §28.13) is the same declaration.  So the
// inline recipient is a `T?` local, and an assignment to a `T` local is a
// R? → T narrowing which the spec forbids.
//
// Before the fix: the OwningSet arm was missing from both the `out var`
// posElem boolean table (for plain `out var`) and the `out using var` arm
// chain.  The recipient's declType stayed null.  A null-typed local accepts
// every assignment (soundness hole), so `var s: R := b11;` was accepted.
//
// After the fix: both tables carry the OwningSet.TryPop arm.  The recipient
// is typed R? and `var s: R := b11;` is correctly rejected.
//
// SIBLING AUDIT (proved on r23-c28 base, pre-fix):
//   OwningArray<TryPopFirst>      — arm already present, sibling AOT controls pass
//   OwningArray<TryPopLast>       — arm already present, sibling AOT controls pass
//   OwningLinkedList<TryPopFirst>  — arm already present, sibling AOT controls pass
//   OwningLinkedList<TryPopLast>   — arm already present, sibling AOT controls pass
//   OwningDictionary<TryPopValue>  — arm already present, sibling AOT controls pass
//
// All siblings remain green (positive controls).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Common class preamble: a disposable class with a stored int field.
const char kRClass[] =
    "class R "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is R then var b := other as R; return b!.X = X; end; "
    "    return false; end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end R; ";

}  // namespace

// =====================================================================
// ACCEPT (positive controls) — these use a pre-declared R? recipient so
// the inline derivation is bypassed.  They verify the narrowing path works
// and serve as baseline that the programs compile and run.
// NOTE: OwningSet.TryPop does not lower (sibling #5361 codegen issue), so
// we cannot use OwningSet here.  The sibling positive controls below use
// OwningArray/OwningLinkedList/OwningDictionary which do lower correctly.
// ========================================================================

// =====================================================================
// REJECT — the core of the fix: inline `out using var` recipient is typed
// R? (after the fix), so R? → R narrowing is rejected.  Before the fix,
// the recipient was untyped and accepted every assignment (including this
// forbidden narrowing).
// ========================================================================

// REJECT — inline `out using var`: R? → R narrowing is forbidden (§7.4).
// Before fix: b11 untyped → `var s: R := b11;` accepted (soundness hole).
// After fix:  b11 typed R? → `var s: R := b11;` rejected.
BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           out_using_var_rejects_rnarrowing_to_r_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); oset.Add(move (new R(9))); "
        "    if oset.TryPop(new R(9), out using var b11) then "
        "      var s: R := b11; "  // R? → R, rejected
        "      return s.X; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7"));
}

// REJECT — plain `out var`: same narrowing check.
BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           plain_out_var_rejects_rnarrowing_to_r_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); oset.Add(move (new R(9))); "
        "    if oset.TryPop(new R(9), out var b11) then "
        "      var s: R := b11; "  // R? → R, rejected
        "      return s.X; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7"));
}

// =====================================================================
// REJECT — sibling collections: R? → R narrowing must also be rejected
// (arms already present, so these are stable controls that must not regress).
// ========================================================================

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owningarray_trypopfirst_rejects_rnarrowing_to_r) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oa := new OwningArray<R>(); oa.Add(move (new R(10))); "
        "    if oa.TryPopFirst(out using var b1) then "
        "      var s: R := b1; "  // R? → R, rejected
        "      return s.X; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "nullable"));
}

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owninglinkedlist_trypoplast_rejects_rnarrowing_to_r) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oll := new OwningLinkedList<R>(); oll.Add(move (new R(8))); "
        "    if oll.TryPopLast(out using var b2) then "
        "      var s: R := b2; "  // R? → R, rejected
        "      return s.X; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "nullable"));
}

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owningdictionary_trypopvalue_rejects_rnarrowing_to_r) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var od := new OwningDictionary<string, R>(); "
        "    od[\"k\"] := move (new R(11)); "
        "    if od.TryPopValue(\"k\", out using var b3) then "
        "      var s: R := b3; "  // R? → R, rejected
        "      return s.X; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "nullable"));
}

// =====================================================================
// ACCEPT (positive controls) — sibling collections with pre-declared R?
// recipients, verifying the narrowing path works (baseline, must not regress).
// ========================================================================

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owningarray_trypopfirst_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oa := new OwningArray<R>(); oa.Add(move (new R(10))); "
        "    var b1: R? := null; "
        "    if oa.TryPopFirst(out b1) then "
        "      if b1 <> null then return b1!.X; end; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owninglinkedlist_trypoplast_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oll := new OwningLinkedList<R>(); oll.Add(move (new R(8))); "
        "    var b2: R? := null; "
        "    if oll.TryPopLast(out b2) then "
        "      if b2 <> null then return b2!.X; end; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owningdictionary_trypopvalue_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var od := new OwningDictionary<string, R>(); "
        "    od[\"k\"] := move (new R(11)); "
        "    var b3: R? := null; "
        "    if od.TryPopValue(\"k\", out b3) then "
        "      if b3 <> null then return b3!.X; end; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// =====================================================================
// LEAK CONTROLS — `out using var` binds the recipient in a using scope.
// The 200× loop exercises the disposal path for the draining collection.
// NOTE: these require #5312 (shape-(a) narrowing) to access bx.X after the
// call.  They are omitted from this test file until #5312 is available.
// The positive controls above (with pre-declared nullable recipients and
// explicit null-check narrowing) verify the basic functionality.
// ========================================================================
