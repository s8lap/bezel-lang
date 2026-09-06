// fix #3906 (§35.11 Generics, v0.294): a generic choice's UNMARKED bare-
// type-parameter payload classified borrow-bearing even at a VALUE
// substitution (`T = int`), so `.Equals`/`.GetHashCode`/`?.Equals`/
// `?.GetHashCode` on `Pair<int>` (payload `value: T`, no `owned` marker) drew
// §35.7's "not auto-hashable" use-site error — and, MEASURED on this worktree
// before the fix, the SAME call sites also misclassified the stdlib's
// `owned`-marked `Result<T, E>` as OWNING at `Result<int, string>` (a value
// substitution the `owned` marker should erase over, §28.1) rather than
// value-like. §35.11's operative sentence:
//
//   "Classification is evaluated per instantiation under §3.6 rule 2 ...
//   An unmarked bare-type-parameter payload is classification-neutral at
//   value and runtime-managed substitutions and borrow-carrying at class
//   and interface substitutions."
//
// ── ROOT CAUSE ───────────────────────────────────────────────────────────
//
// `ExprTC::checkEqualityMemberUseSite` (ExprTypecheck_Calls.cpp) is the
// shared use-site gate for a direct `.Equals`/`.GetHashCode` (`.` form) or
// `?.Equals`/`?.GetHashCode` (`?.` form) call. For a choice receiver it built
// a `TypeRef rt` carrying ONLY `rt.declaration = targetType` — the choice's
// bare TypeSymbol, with `rt.genericArgs` left EMPTY — and fed that `rt` to
// `isAutoEqualityBearingChoice` / `choiceHasUserDeclaredEquality` /
// `classifyChoiceType`. `Pair<int>`'s actual instantiation argument (`int`)
// never reached the classifier: `classifyChoiceCore` (common/
// OwnershipFacts.cpp) seeds its per-instantiation substitution env
// (`seedRecordSubstEnv`) from `rt->genericArgs`, which was empty, so the
// borrow-carrying leg's bare-parameter case
// (`payloadFieldTypeIsBorrowCarrying`) fell through to its documented
// conservative "uninstantiated / still-generic bare param" answer (borrow-
// carrying) instead of resolving `T` to `int`. The SAME missing genericArgs
// also broke the OWNING leg's `substitutedChoicePayloadType` for an `owned`-
// marked payload (`Result<T,E>`'s `Ok(owned value: T)`), which needs
// `rt->genericArgs` to erase the marker over a value substitution (§28.1) —
// MEASURED pre-fix: `Result<int,string>.Equals(...)` was rejected with
// "the choice is owning", not the filed issue's "borrow-bearing" wording,
// but the SAME missing-instantiation root cause.
//
// Every OTHER consumer of this predicate set already threads the receiver's
// real resolved TypeRef through (the sibling `=`/`<>` use-site check in
// ExprTypecheck_Exprs.cpp reads `ctx_.exprType(*operand)`; `isEqualityBearing`
// and `checkHashContainerKeys` in SymbolCollection.cpp / TypeResolution_Checks
// .cpp read the concrete key TypeRef at its declared site) — this was the
// ONE call site that discarded it (CLAUDE.md §6: an existing authority the
// compiler already computes for "is this receiver's concrete instantiation
// borrow-carrying/owning/value-like").
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// `checkEqualityMemberUseSite` gained an optional `recvTy` parameter — the
// receiver's own resolved TypeRef (`ctx_.exprType` of the receiver
// expression), threaded from both call sites (`resolveMemberAccess` for the
// `.` form via `ma.target`; `resolveNullCondMember` for the `?.` form via the
// already-computed `recvTyForDiag`). When it names the same declaration as
// `targetType` it supplies `rt`'s `genericArgs`, exactly mirroring the
// `=`/`<>` use-site check's own `opTy`-vs-bare fallback pattern; otherwise the
// prior bare-declaration behavior is unchanged (so an unresolved/absent
// receiver type still gets the conservative reading).
//
// ── WHAT EACH ROW IS FOR ─────────────────────────────────────────────────────
//
//   T1  the filed repro (`Pair<int>`, unmarked bare payload `value: T`):
//       `.Equals` now compiles and returns the CORRECT runtime answer for
//       both an equal-payload pair (True) and a differing-payload pair
//       (False) — not just "sema now accepts it".
//   T2  `.GetHashCode()` at the same value substitution: two equal `Pair<int>`
//       values hash equal (§8.3), proving the accepted program's hash path
//       — not only its equality path — is correct, not merely un-rejected.
//   T3  the stdlib `owned`-marked twin: `Result<int, string>.Equals` — MEASURED
//       broken pre-fix by the SAME call site (see root-cause note above),
//       confirming the fix's reach is not confined to the unmarked-bare-
//       parameter leg the issue titled.
//   T4  the `?.` form (`resolveNullCondMember`) at a value substitution:
//       `Pair<int>?` receiver, `?.Equals` — the second call site this fix
//       touches, exercised independently of T1's `.` form.
//   T5  the per-instantiation split at TWO independent bare type parameters
//       in one choice (`Either<T, U>`, one unmarked payload per case) at
//       `Either<int, string>` — both `T` and `U` are value substitutions and
//       must BOTH classify neutral in the same instantiation, pinning that
//       `seedRecordSubstEnv`'s per-index substitution is correct, not just
//       the single-parameter case.
//   T6  NEGATIVE CONTROL (the axis that must stay rejected): the SAME choice
//       shape (`Pair<T>`, unmarked bare payload) at a CLASS substitution
//       (`Pair<Box>`) is genuinely borrow-carrying per §35.11's OTHER half
//       ("borrow-carrying at class and interface substitutions") and must
//       still draw the exact §35.7 diagnostic, both for `.Equals` (T6a) and
//       `?.Equals` (T6b) — proving the fix relaxed ONLY the value/runtime-
//       managed axis, not the class axis.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasErrDiagWith(const bezel::test::JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ── T1 — value substitution, unmarked bare payload: `.Equals` accepts AND
// answers correctly for both an equal and a differing payload.
BEZEL_TEST(codegen_generic_choice_bare_typeparam_value_subst_3906,
           pair_int_equals_value_substitution_correct_answer_spec_35_11) {
    auto r = runAot(
        "module M3906A; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class C "
        "  public static method Main(): int "
        "    var a: Pair<int> := Pair<int>.First(3); "
        "    var b: Pair<int> := Pair<int>.First(3); "
        "    var c: Pair<int> := Pair<int>.First(4); "
        "    Console.WriteLine(\"eq \" + a.Equals(b).ToString()); "
        "    Console.WriteLine(\"ne \" + a.Equals(c).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq True\nne False\n"));
}

// ── T2 — value substitution, unmarked bare payload: `.GetHashCode()` for two
// equal `Pair<int>` values agrees (§8.3) — the hash path, not just equality.
BEZEL_TEST(codegen_generic_choice_bare_typeparam_value_subst_3906,
           pair_int_gethashcode_value_substitution_agrees_spec_8_3) {
    auto r = runAot(
        "module M3906B; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class C "
        "  public static method Main(): int "
        "    var a: Pair<int> := Pair<int>.First(3); "
        "    var b: Pair<int> := Pair<int>.First(3); "
        "    Console.WriteLine(\"hash \" "
        "      + (a.GetHashCode() = b.GetHashCode()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("hash True\n"));
}

// ── T3 — stdlib `owned`-marked twin, same call site: `Result<int, string>`
// (§28.1 marker erasure over a value substitution). MEASURED broken pre-fix
// with "the choice is owning" (not the issue's "borrow-bearing" wording) —
// the SAME missing-`genericArgs` root cause reaching the OWNING leg's
// `substitutedChoicePayloadType` instead of the borrow-carrying leg.
BEZEL_TEST(codegen_generic_choice_bare_typeparam_value_subst_3906,
           result_int_string_equals_owned_marker_erasure_spec_28_1) {
    auto r = runAot(
        "module M3906C; "
        "class C "
        "  public static method Main(): int "
        "    var a: Result<int, string> := Result<int, string>.Ok(3); "
        "    var b: Result<int, string> := Result<int, string>.Ok(3); "
        "    Console.WriteLine(\"eq \" + a.Equals(b).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("eq True\n"));
}

// ── T4 — the `?.` form (resolveNullCondMember), value substitution: the
// SECOND call site this fix touches, exercised independently of T1.
BEZEL_TEST(codegen_generic_choice_bare_typeparam_value_subst_3906,
           pair_int_nullcond_equals_value_substitution_spec_7_5) {
    auto r = runAot(
        "module M3906D; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class C "
        "  public static method Main(): int "
        "    var a: Pair<int>? := Pair<int>.First(3); "
        "    var b: Pair<int> := Pair<int>.First(3); "
        "    var eq := a?.Equals(b); "
        "    Console.WriteLine(\"nc \" + (eq ?? false).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nc True\n"));
}

// ── T5 — two INDEPENDENT bare type parameters in one choice, each unmarked
// in a different case, both substituted at value types in the SAME
// instantiation (`Either<int, string>`) — pins that the per-instantiation
// substitution env indexes both parameters correctly, not just a single-
// parameter shape.
BEZEL_TEST(codegen_generic_choice_bare_typeparam_value_subst_3906,
           either_two_bare_typeparams_both_value_substituted_spec_35_11) {
    auto r = runAot(
        "module M3906E; "
        "choice Either<T, U> Left(value: T), Right(value: U); end Either; "
        "class C "
        "  public static method Main(): int "
        "    var a: Either<int, string> := Either<int, string>.Left(3); "
        "    var b: Either<int, string> := Either<int, string>.Left(3); "
        "    var c: Either<int, string> := Either<int, string>.Right(\"x\"); "
        "    var d: Either<int, string> := Either<int, string>.Right(\"x\"); "
        "    Console.WriteLine(\"left \" + a.Equals(b).ToString()); "
        "    Console.WriteLine(\"right \" + c.Equals(d).ToString()); "
        "    Console.WriteLine(\"cross \" + a.Equals(c).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("left True\nright True\ncross False\n"));
}

// ── T6a — NEGATIVE CONTROL: the SAME choice shape at a CLASS substitution
// (§35.11's other half — "borrow-carrying at class and interface
// substitutions") must still draw the exact §35.7 use-site diagnostic for
// the `.` form. Proves the fix relaxed only the value/runtime-managed axis.
BEZEL_TEST(codegen_generic_choice_bare_typeparam_value_subst_3906,
           pair_box_class_substitution_still_borrow_bearing_dot_form_spec_35_11) {
    auto r = runAot(
        "module M3906F; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class Box public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(3); "
        "    var a: Pair<Box> := Pair<Box>.First(box); "
        "    var b: Pair<Box> := Pair<Box>.First(box); "
        "    var eq := a.Equals(b); "
        "    Console.WriteLine(eq.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The diagnostic TEXT matters: a value-like/owning misclassification
    // would also make `r.ran` false in some other broken way. Assert the
    // EXACT §35.7 borrow-bearing wording so this control cannot pass for the
    // wrong reason.
    BEZEL_EXPECT_TRUE(hasErrDiagWith(
        r, "it is not auto-hashable (\xC2\xA7""35.7) \xE2\x80\x94 the choice "
           "is borrow-bearing"));
}

// ── T6b — the SAME negative control through the `?.` form (the second call
// site this fix touches) — a class substitution must stay rejected there
// too.
BEZEL_TEST(
    codegen_generic_choice_bare_typeparam_value_subst_3906,
    pair_box_class_substitution_still_borrow_bearing_nullcond_form_spec_35_11) {
    auto r = runAot(
        "module M3906G; "
        "choice Pair<T> First(value: T), Second(); end Pair; "
        "class Box public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var box := new Box(3); "
        "    var a: Pair<Box>? := Pair<Box>.First(box); "
        "    var b: Pair<Box> := Pair<Box>.First(box); "
        "    var eq := a?.Equals(b); "
        "    Console.WriteLine((eq ?? false).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(
        r, "it is not auto-hashable (\xC2\xA7""35.7) \xE2\x80\x94 the choice "
           "is borrow-bearing"));
}
