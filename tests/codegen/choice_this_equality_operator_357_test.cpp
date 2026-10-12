// fix #357 — a choice that DECLARES `=`/`<>` was rejected with §35.7's
// use-site "not available on choice" error when the operand was spelled
// `this`, and then failed codegen IR verification once that rejection was
// lifted (spec §22.6, §25.2, §35.7 Equality, hashing, `ToString` — v0.334).
//
// ── MEASURED on the pre-fix tree (this worktree, at 7da42fdc) ───────────────
//
// A choice `Outcome` declaring `operator =`, `operator <>`, `Equals`,
// `GetHashCode`, inside a method `Cmp(other: Outcome)`:
//
//   var r := this = other;      // 2 errors:
//     `=` is not available on choice `Outcome`: it is not auto-hashable
//     (§35.7) — the choice is owning, and a non-value-like choice carries
//     no value equality. Declare `=` and `<>` overloads (with
//     `Equals`/`GetHashCode`), or restructure the choice payload...
//
// The diagnostic is SELF-CONTRADICTING: it says "Declare `=` and `<>`
// overloads" about a choice that declares exactly those. `other = this`
// fails identically (2 errors). The isolating matrix, all measured:
//
//   `a = b` inside Main                         CLEAN
//   `var t := this; var r := t = other;`        CLEAN
//   `var r := this = other;`                    2 errors
//   `var r := other = this;`                    2 errors
//
// ── ROOT CAUSE — TWO LINKED LAYERS ──────────────────────────────────────────
//
// LAYER 1 (sema, ExprTypecheck_Exprs.cpp findBinaryOperator): the resolver
// deliberately leaves `this`/`base` WITHOUT an exprType ("Slice 3b ...
// leave them unresolved" — only a PRIMITIVE-extension body gets one).
// `memberTargetType` resolves the spelling to `currentType_`, which is why
// `collectInheritedOperators` DID find the declared `=`; but the per-position
// match reads `ctx_.exprType` directly, so `quality()` returned
// `Inapplicable`, the candidate set emptied, and §35.7's use-site fallback
// fired on a choice whose operators it declares. Fix: a `staticTypeOf`
// recovery inside findBinaryOperator, keyed on the `this`/`base` SPELLING.
//
// LAYER 2 (codegen, LlvmBackend_Layout.cpp coerceThisRecordValue): with layer
// 1 in place the operator dispatches and codegen then fails
//   Call parameter type does not match function signature!
//    %choice.Outcome = type { i32, { i32 }, { ptr, i1 } }
//     %4 = call i1 @"bz$Conf.Outcome$op$="(ptr %0, %choice.Outcome %4)
// `this` lowers to `ptr` (CG29) but the by-value parameter wants the struct.
// `coerceThisRecordValue` is exactly this fix for RECORDS (#1759) — its
// guard `currentReceiverRecordType()` returned null for a Choice parent, so a
// choice's `this` never loaded. Fix: admit a Choice parent and use
// `choiceStruct` for it.
//
// ── LAYER 2 IS PRE-EXISTING AND INDEPENDENT OF LAYER 1 (measured) ───────────
//
// It reproduces with NO binary operator anywhere in the program, so
// `findBinaryOperator` is never reached:
//   choice Outcome ... public method IsSelf(other: Outcome): bool
//                                 return other.GetHashCode() = 7;
//                     public method SelfArg(): bool => this.IsSelf(this);
//   -> Call parameter type does not match function signature!
// Also measured: a local-bound argument (`var a := this; a.IsSelf(a)`) was
// CLEAN, a value-LIKE choice fails identically to an owning one, and
// `return this` was already CLEAN. The trigger is precisely `this` in a
// BY-VALUE ARGUMENT position on a choice receiver — not owning-ness.
//
// (`this.Cmp(this)`, the shape named in the original dispatch, is unbounded
// self-recursion and segfaults on ANY correct compiler; the isolation probe
// used it only to read the IR-verification message out of `bezelc`, which
// rejects before the program ever runs.)
//
// ── WHAT EACH ROW IS FOR ─────────────────────────────────────────────────────
//
//   T1  layer 1 + layer 2 together: `this = other` on a DECLARING choice runs
//       and prints the operator's own answer. Pre-fix: 2 §35.7 errors.
//   T2  the DISCRIMINATING control for T1 — the operator body compares
//       payloads, so True and False are both produced by the DECLARED
//       operator and not by any constant. A green T1 with a constant `true`
//       operator would print True/True; T2 prints True/False.
//   T3  the reverse operand order `other = this` (the same 2-error row).
//   T4  layer 2 on its own, NO binary operator in the program: `this.Cmp(this)`
//       compiles and runs. This row fails on the pre-fix tree with an IR
//       verification failure even if layer 1 were already fixed.
//   T5  the pre-fix CLEAN rows stay clean — `a = b` in Main and
//       `var t := this; t = other` — so the layer-1 fix did not break the
//       spellings that already worked.
//   T6  §35.7's rejection still fires where it SHOULD: a choice that declares
//       NO `=`/`<>` overload, compared with `this` as an operand. The fix must
//       not turn the use-site error off for the spelling that draws it.

#include <cstring>
#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// A value-like choice carrying the equality surface, so the ONLY thing that
// can answer `this = other` is the DECLARED operator. `CodeOf` reads the
// active case's payload, so the operator's answers differ per operand pair.
static const char* kDeclaringChoice =
    "module M357A; "
    "choice Outcome "
    "  Won(code: int), "
    "  Lost(code: int); "
    "  private method CodeOf(): int "
    "    var res := 0; "
    "    case this of "
    "      Outcome.Won(c) => res := c;, "
    "      Outcome.Lost(c) => res := c "
    "    end case; "
    "    return res; "
    "  end CodeOf; "
    "  public static method operator =(a: Outcome, b: Outcome): bool "
    "    => a.CodeOf() = b.CodeOf(); "
    "  public static method operator <>(a: Outcome, b: Outcome): bool "
    "    => a.CodeOf() <> b.CodeOf(); "
    "  public method Equals(other: Outcome): bool "
    "    => this.CodeOf() = other.CodeOf(); "
    "  public method GetHashCode(): int => 7; "
    "  public method SameAsThis(other: Outcome): bool => this = other; "
    "  public method ThisNotThis(other: Outcome): bool => other = this; "
    "end Outcome; ";

// ── T1 — the fix's headline row (§35.7, §25.2, §22.6). Pre-fix this program
// is REJECTED with two §35.7 "not available on choice" errors naming
// operators the choice declares. Post-fix it runs.
BEZEL_TEST(codegen_choice_this_equality_operator_357,
           choice_declaring_equals_operator_is_admitted_at_this_use_site_spec_35_7_25_2) {
    auto r = runAot(
        std::string(kDeclaringChoice) +
        "class C "
        "  public static method Main(): int "
        "    var a := Outcome.Won(5); "
        "    var b := Outcome.Won(5); "
        "    Console.WriteLine(a.SameAsThis(b).ToString()); "
        "    Console.WriteLine(\"END\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nEND\n"));
}

// ── T2 — the DISCRIMINATING control. The declared operator compares payloads,
// so these two calls must disagree. A `true`-constant operator (or a green T1
// produced by any path that never reaches the operator body) would print
// True/True and fail here.
BEZEL_TEST(codegen_choice_this_equality_operator_357,
           declared_operator_body_runs_at_this_use_site_spec_25_2) {
    auto r = runAot(
        std::string(kDeclaringChoice) +
        "class C "
        "  public static method Main(): int "
        "    var a := Outcome.Won(5); "
        "    var b := Outcome.Won(5); "
        "    var c := Outcome.Won(9); "
        "    Console.WriteLine(a.SameAsThis(b).ToString()); "
        "    Console.WriteLine(a.SameAsThis(c).ToString()); "
        "    Console.WriteLine(\"END\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nEND\n"));
}

// ── T3 — the reverse operand order. `other = this` was the second of the two
// 2-error rows, and it is a distinct call site from T1 (`this` in position 1
// rather than position 0), so it pins the fix on both operand positions.
BEZEL_TEST(codegen_choice_this_equality_operator_357,
           choice_declaring_equals_operator_is_admitted_with_this_on_the_right_spec_35_7) {
    auto r = runAot(
        std::string(kDeclaringChoice) +
        "class C "
        "  public static method Main(): int "
        "    var a := Outcome.Won(5); "
        "    var b := Outcome.Won(9); "
        "    Console.WriteLine(a.ThisNotThis(b).ToString()); "
        "    Console.WriteLine(\"END\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nEND\n"));
}

// ── T4 — LAYER 2 ALONE, with NO binary operator anywhere in the program.
// findBinaryOperator is never reached on this path, so this row measures the
// codegen defect independently of the sema one. `SelfArg` passes the choice
// receiver as a BY-VALUE `Outcome` argument — the shape the pre-fix compiler
// dropped to a raw `ptr`, failing IR verification ("Call parameter type does
// not match function signature!", `ptr` where `%choice.Outcome` is wanted).
// `SelfArgLocals` is the control that was already CLEAN (the same value bound
// to a local first) and must stay clean.
//
// NOTE: `this.Cmp(this)` — a shape measured during the investigation — is
// deliberately NOT used: it is unbounded self-recursion, and a stack-overflow
// segfault is the CORRECT outcome of it, which would make this row a crash
// oracle rather than a value one. `SelfArg` reaches the identical lowering
// path through a non-recursive callee.
BEZEL_TEST(codegen_choice_this_equality_operator_357,
           choice_this_by_value_argument_lowers_as_a_value_spec_35_7_9_4) {
    auto r = runAot(
        "module M357B; "
        "choice Outcome "
        "  Won(code: int), "
        "  Lost(code: int); "
        "  public method Equals(other: Outcome): bool => true; "
        "  public method GetHashCode(): int => 7; "
        "  public method IsSelf(other: Outcome): bool "
        "    return other.GetHashCode() = 7; "
        "  end IsSelf; "
        "  public method SelfArg(): bool => this.IsSelf(this); "
        "  public method SelfArgLocals(): bool "
        "    var a := this; "
        "    return a.IsSelf(a); "
        "  end SelfArgLocals; "
        "end Outcome; "
        "class C "
        "  public static method Main(): int "
        "    var a := Outcome.Won(1); "
        "    var b := Outcome.Won(1); "
        "    Console.WriteLine(a.SelfArg().ToString()); "
        "    Console.WriteLine(a.SelfArgLocals().ToString()); "
        "    Console.WriteLine(\"END\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nEND\n"));
}

// ── T5 — the pre-fix CLEAN spellings must stay clean. `a = b` in Main and
// `var t := this; t = other` both resolved before the fix (their operands
// carry an exprType); the layer-1 recovery must not disturb either.
BEZEL_TEST(codegen_choice_this_equality_operator_357,
           choice_equality_via_locals_and_in_main_is_unaffected_spec_35_7) {
    auto r = runAot(
        std::string(kDeclaringChoice) +
        "class C "
        "  public static method Main(): int "
        "    var a := Outcome.Won(5); "
        "    var b := Outcome.Won(5); "
        "    var c := Outcome.Won(9); "
        "    Console.WriteLine((a = b).ToString()); "
        "    Console.WriteLine((a = c).ToString()); "
        "    Console.WriteLine(\"END\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nEND\n"));
}

// ── T6 — §35.7's use-site rejection must STILL fire where it should. A choice
// that declares NO `=`/`<>` overload has nothing for `a = b` to invoke
// (§35.7: "The `=`/`<>` half stays keyed on the operators, there being nothing
// for `a = b` to invoke without an overload"), so a NON-auto-hashable such
// choice draws the error — including when the operand is spelled `this`. The
// layer-1 fix widens which OPERATORS are found; it must not admit the
// no-operator case.
BEZEL_TEST(codegen_choice_this_equality_operator_357,
           choice_without_equals_operator_still_rejected_at_this_use_site_spec_35_7) {
    auto r = runAot(
        "module M357C; "
        "class Conn "
        "  public method Close() "
        "  end Close; "
        "end Conn; "
        "choice Outcome "
        "  Won(code: int), "
        "  Lost(owned conn: Conn, tag: bool); "
        "  public method Equals(other: Outcome): bool => true; "
        "  public method GetHashCode(): int => 7; "
        "  public method Cmp(other: Outcome): bool => this = other; "
        "end Outcome; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    // Assert the SPECIFIC §35.7 diagnostic, not merely that compilation
    // failed — a row that "rejects" for an unrelated reason is green here and
    // would hide a regression that silently un-rejected the use site.
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "not available on choice"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
