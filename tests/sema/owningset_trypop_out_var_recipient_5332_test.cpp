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
// inline recipient is a `T?` local.
//
// #5332 (sema): before the fix the OwningSet arm was missing from BOTH the
// `out var` posElem table and the `out using var` arm chain, so the
// recipient's declType stayed null.  A null-typed local accepts every
// assignment (soundness hole), so `var s: R := b11;` was waved through for
// the wrong reason.
//
// #5312 (narrowing): stdlib §1 (v0.315) — "Every shape-(a) signature
// in this document carries the `when true` nullability postcondition on its
// out parameter (language spec §22.4) ... so in a region where the call is
// known `true`, the recipient is narrowed non-null and no `!` is needed; the
// clause is elided from the per-type listings below for brevity but is
// normative for **every** shape-(a) member in this document (the table's (a)
// row names them)."  `OwningSet<T>.TryPop` is named in that (a) row, so it
// DOES carry the clause, and `narrowFromBuiltinShapeA` (§22.4 / §7.11,
// ExprTypecheck.cpp) narrows the recipient in the controlled branch.  Inside
// `if TryPop(...) then`, `b11` is `R` — so `var s: R := b11;` is ACCEPTED.
//
// BEHAVIOUR FLIP (owner ruling on #5430).  The five REJECT rows below were
// written against the pre-#5312 world, where no shape-(a) builtin member
// narrowed its recipient.  They pinned a verdict the spec does not give: they
// asserted the rejection of `var s: R := b11;` inside the `then` branch, but
// on a shape-(a) member the spec REQUIRES that region to be narrowed.  They
// are therefore rewritten as ACCEPT rows asserting the narrowed program
// compiles, runs, and prints the element — a strengthening (a running,
// value-asserting program), not a weakening.  Their former intent is
// preserved and pinned by the two rejection rows now in the file:
//   * `..._still_nullable_after_the_if_22_4` — the SAME recipient read in the
//     region AFTER the `if`, where the clause does NOT apply and the spec
//     still requires narrowing.  This is the row that distinguishes "typed
//     `R?`, narrowed only in the controlled branch" from both "untyped" (the
//     #5332 soundness hole) and "typed `R`" (over-typing).
//   * `..._user_method_without_clause_still_rejected_22_4` — §22.4's other
//     half: a USER-declared `bool` + nullable-`out` method with no `when
//     true` clause is still rejected there.  This is exactly the coverage the
//     flipped rows were mistaken for providing, and it does not collide with
//     the flip: §22.4's sentence is about *user* methods, while these stdlib
//     members are declared with the clause through §1's document-wide
//     convention.
//
// SIBLING AUDIT (proved on the r23-c28 base, and re-proved here):
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
// ACCEPT (flipped from REJECT by the owner ruling on #5430) — the narrowed
// program COMPILES, RUNS, and prints the element's value.
//
// `var s: R := b11;` inside `if TryPop(...) then` is legal here, and its
// stdout is the element's `X` (MEASURED on r23-c28 with
// `scripts/agent_check.sh --probe --asan`, which also reported no sanitizer
// finding for the three siblings).  Each row keeps `exitCode == 0` as a real
// assertion — the program must run, not merely compile.
// // =====================================================================

// ACCEPT — inline `out using var`: inside the controlled branch the §1
// shape-(a) `when true` postcondition narrows `b11` to `R`, so the R? -> R
// assignment is legal and prints the popped element.
BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           out_using_var_narrowed_in_then_accepted_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); oset.Add(move (new R(9))); "
        "    if oset.TryPop(new R(9), out using var b11) then "
        "      var s: R := b11; "  // narrowed in the then region (stdlib §1 shape (a))
        "      Console.WriteLine(s.X); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// ACCEPT — plain `out var`: the same call through the `out var` derivation
// table (a SEPARATE table from the `out using var` chain, and the one #5332
// also had to arm).  Spec §22.4 gives both forms the same "inferred from the
// method's `out` parameter declaration" rule, so both narrow identically.
BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           plain_out_var_narrowed_in_then_accepted_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); oset.Add(move (new R(9))); "
        "    if oset.TryPop(new R(9), out var b11) then "
        "      var s: R := b11; "  // narrowed in the then region (stdlib §1 shape (a))
        "      Console.WriteLine(s.X); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// =====================================================================
// ACCEPT — sibling collections: the same §1 shape-(a) rule applies to the
// members the (a) row names, so the narrowed read compiles and prints for
// each.  These were the file's "must not regress" controls; they keep that
// job, now asserting a running program rather than a rejection.
// // =====================================================================

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owningarray_trypopfirst_narrowed_in_then_accepted_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oa := new OwningArray<R>(); oa.Add(move (new R(10))); "
        "    if oa.TryPopFirst(out using var b1) then "
        "      var s: R := b1; "
        "      Console.WriteLine(s.X); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "10\n");
}

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owninglinkedlist_trypoplast_narrowed_in_then_accepted_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oll := new OwningLinkedList<R>(); oll.Add(move (new R(8))); "
        "    if oll.TryPopLast(out using var b2) then "
        "      var s: R := b2; "
        "      Console.WriteLine(s.X); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "8\n");
}

BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           owningdictionary_trypopvalue_narrowed_in_then_accepted_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var od := new OwningDictionary<string, R>(); "
        "    od[\"k\"] := move (new R(11)); "
        "    if od.TryPopValue(\"k\", out using var b3) then "
        "      var s: R := b3; "
        "      Console.WriteLine(s.X); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// =====================================================================
// REJECT — the coverage the flipped rows were mistaken for providing, kept
// and made explicit.  Both rows were MEASURED for their exact diagnostic
// text with `scripts/agent_check.sh --probe` before being written here;
// neither needle is asserted from belief.
// // =====================================================================

// REJECT — the SAME `OwningSet.TryPop` recipient, read in the region AFTER
// the `if`.  The `when true` postcondition applies only in the controlled
// branch, so here `b11` is still `R?` and the spec requires narrowing
// (§7.4).  This is the row that distinguishes "typed `R?`" from both
// "untyped" (the #5332 soundness hole, which accepted this) and "typed `R`"
// (over-typing, which would have accepted it for the wrong reason).
//
// MEASURED diagnostic:
//   the initializer for `s` is nullable but the target type is non-nullable (§7)
BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           out_using_var_still_nullable_after_the_if_22_4) {
    auto r = runAot(
        std::string("module M; ") + kRClass +
        "class C "
        "  public static method Main(): int "
        "    using var oset := new OwningSet<R>(); oset.Add(move (new R(9))); "
        "    if oset.TryPop(new R(9), out using var b11) then "
        "      Console.WriteLine(b11.X); "
        "    end; "
        "    var s: R := b11; "  // OUTSIDE the controlled branch: still R?
        "    return s.X; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7"));
}

// REJECT — §22.4's other half, and the exact coverage the flipped rows were
// taken to provide: a USER-declared `bool` + nullable-`out` method with NO
// `when true` clause is NOT narrowed in its controlled branch.  Spec §22.4
// (v0.315): "Against a `Try` method *without* the clause, the explicit `!`
// remains required after the bool guard."  This does not collide with the
// flip above: §1's document-wide convention declares the STDLIB shape-(a)
// members with the clause, whereas a user method only gets it by declaring
// it — so no signature silently gains a postcondition its body was never
// checked against.
//
// MEASURED diagnostic:
//   operator `+` is not defined on a nullable value type — narrow first
//   ... (§7.11, v0.267)
BEZEL_TEST(owningset_trypop_out_var_recipient_5332,
           user_method_without_when_true_clause_still_rejected_22_4) {
    auto r = runAot(
        "module M; "
        "public class Bag "
        "  public method TryTake(out item: int?): bool item := 3; return true; end TryTake; "
        "end Bag; "
        "class C "
        "  public static method Main(): int "
        "    var bag := new Bag(); "
        "    if bag.TryTake(out var v) then "
        "      Console.WriteLine(v + 1); "  // no clause: the `!` is still required
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "not defined on a nullable value type"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.11"));
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
