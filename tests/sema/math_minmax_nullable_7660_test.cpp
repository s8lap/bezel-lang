// Spec-derived regression test for issue #7660 — an UN-NARROWED NULLABLE
// argument at a `Math.Min` / `Math.Max` / `Math.Clamp` position was refused
// with CODEGEN's fail-closed COMPARISON text instead of the house §7
// NULLABILITY text.
//
// ─── THE NORMATIVE SENTENCES (docs/, v0.355) ────────────────────────────────
//
// stdlib §2.2 (`Math`):
//
//     public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
//     public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
//     public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;
//
// spec §14.2 "Constraints" (v0.355): "A type-bound constraint — naming a
// class or interface — implies non-null T. A nullable type `MyClass?` or
// `IFoo?` is distinct from its non-nullable form (§7.1) and a `null` value
// satisfies no interface contract, so a class or interface bound admits only
// non-nullable substitutions, as `where T: class` does; inside the generic
// body, T is non-nullable and `T?` is its nullable companion."
//
//   ⇒ `T` is a NON-NULLABLE type, so `a` / `b` / `value` / `low` / `high`
//     are NON-NULLABLE PARAMETERS. The standard library spells them without
//     a `?`, which is exactly what the ONE built-in parameter table's
//     `nonNullable` column transcribes.
//
// spec §7.3 "Assigning null" (v0.355): "Assigning `null` to a non-nullable
// target is a compile error."
//
// spec §7.1 "Non-nullable and nullable types" (v0.355): "The compiler
// enforces the distinction at every assignment, parameter pass, and return."
//
// spec §7.4 "Member access on nullable values" (v0.355): a read §7.4 has
// narrowed non-null is of its NON-NULLABLE type — so a narrowed argument is
// legal at the position.
//
// spec §22.6 "Overloading" (v0.355): "The argument's static type is its type
// at the call: a read of a binding §7.4 has narrowed non-null is of its
// non-nullable type."
//
// ─── WHAT THE DEFECT WAS ────────────────────────────────────────────────────
//
// The `Math.Min` / `Math.Max` / `Math.Clamp` rows of the ONE built-in
// parameter table (`src/common/BuiltinRegistry.cpp`'s `kBuiltinParams`) carried
// the §7.3 mask `0x00` — "no position of this form is declared non-nullable"
// — so the §7 nullability gate (`ExprTC::checkBuiltinNullableArgs` →
// `common::builtinParamIsNonNullable`) had NO declared position to claim at
// them. An un-narrowed nullable argument therefore passed sema untouched and
// reached CODEGEN, which reported its own fail-closed guard's text:
//
//     `Math.Min` could not lower a comparison for this argument type
//     (stdlib §2.2 requires `where T: IComparable<T>`): it is not a numeric
//     primitive this intrinsic handles, not `string`, and exposes no
//     `CompareTo` reachable from here. (#2786)
//
// The refusal was RIGHT; the WORDING was the wrong rule's. That text is
// about the COMPARISON and about a type the argument does not have, at a
// call §7.3 refuses for a reason that has nothing to do with ordering.
//
// The SIBLING `Math.Sign` (`where T: INumber<T>` — the same shape: a type
// PARAMETER as the declared parameter type) already carried the §7.3 mask
// `0x01` (since #7227) and therefore already drew the house §7 wording:
// "the argument to `Math.Sign` is nullable but the target type is
// non-nullable (§7)". This file pins the same wording for the three
// selectors.
//
// ─── THE ORACLE ─────────────────────────────────────────────────────────────
//
// The refusals are pinned by their EXACT diagnostic and their LOCATION, and
// by the ABSENCE of the pre-fix text (the comparison message). The legal
// spellings are pinned by their VALUE.
//
// A BEHAVIOUR FLIP this file records: the un-narrowed nullable over
// `Math.Min` / `Math.Max` / `Math.Clamp` was refused with the codegen
// comparison text before the fix and is refused with the §7 nullability text
// now. The REFUSAL is unchanged; only its wording is. No test pinned the old
// wording.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §7 refusal: the program did not run, the driver rejected it at a REAL
// source location (never a crash / signal / timeout / ASan report), and the
// diagnostic carries the house §7 wording for THIS member.
void expectRefusedBySection7(const JitResult& r, const char* member) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the argument to `Math.") + member +
            "` is nullable but the target type is non-nullable (§7)")
                .c_str()));
    // The PRE-FIX text: codegen's fail-closed comparison guard. The refusal
    // is unchanged; its wording is.
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "could not lower a comparison"));
    // The pre-fix symptoms of a codegen-emitted diagnostic are not the
    // sema-emitted one: no crash, no verifier abort.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
}

}  // namespace

// ===========================================================================
// The REFUSALS — the issue's literal repro and its neighbours.
// ===========================================================================

// THE ISSUE'S LITERAL REPRO. `Math.Min(d, 1.0)` with `d: double?`.
BEZEL_TEST(math_minmax_nullable_7660,
           min_nullable_double_is_refused_by_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Min(d, 1.0));"));
    expectRefusedBySection7(r, "Min");
}

// The same shape at `Math.Max`.
BEZEL_TEST(math_minmax_nullable_7660,
           max_nullable_double_is_refused_by_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Max(d, 1.0));"));
    expectRefusedBySection7(r, "Max");
}

// And at `Math.Clamp`, whose `value` position is the first of three.
BEZEL_TEST(math_minmax_nullable_7660,
           clamp_nullable_double_is_refused_by_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Clamp(d, 0.0, 1.0));"));
    expectRefusedBySection7(r, "Clamp");
}

// THE SECOND POSITION: `b` is the same declared parameter (`T`), so the
// refusal is the same one at a different location.
BEZEL_TEST(math_minmax_nullable_7660,
           min_nullable_double_in_second_position_is_refused_by_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Min(1.0, d));"));
    expectRefusedBySection7(r, "Min");
}

// THE OTHER PRIMITIVE CARRIERS: `int?` and `decimal?`.
BEZEL_TEST(math_minmax_nullable_7660,
           min_nullable_int_is_refused_by_7) {
    auto r = runAot(prog(
        "    var i: int? := null; "
        "    Console.WriteLine(Math.Min(i, 1));"));
    expectRefusedBySection7(r, "Min");
}

BEZEL_TEST(math_minmax_nullable_7660,
           min_nullable_decimal_is_refused_by_7) {
    auto r = runAot(prog(
        "    var x: decimal? := null; "
        "    Console.WriteLine(Math.Min(x, 1m));"));
    expectRefusedBySection7(r, "Min");
}

// THE `null` LITERAL: §7.3 refuses it at a non-nullable parameter, and the
// house §7 wording says so.
BEZEL_TEST(math_minmax_nullable_7660, min_null_literal_is_refused_by_7) {
    auto r = runAot(prog("    Console.WriteLine(Math.Min(null, 1.0));"));
    expectRefusedBySection7(r, "Min");
}

// THE NAMED SPELLING: §22.5 binds a named argument to the parameter it
// NAMES, so the refusal must reach the named spelling too.
BEZEL_TEST(math_minmax_nullable_7660,
           min_named_nullable_argument_is_refused_by_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Min(a := d, b := 1.0));"));
    expectRefusedBySection7(r, "Min");
}

// THE EXPLICIT TYPE-ARGUMENT SPELLING: `Math.Min<double>(d, 1.0)` binds `T`
// to `double`, and a `double?` argument converts to no `double` (§5.1's
// nullable lifting is one-way).
BEZEL_TEST(math_minmax_nullable_7660,
           min_explicit_type_argument_nullable_is_refused_by_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Min<double>(d, 1.0));"));
    expectRefusedBySection7(r, "Min");
}

// A NULLABLE PARAMETER never narrows (§7.4: narrowing applies to locals and
// `readonly` fields only), so the read is `double?` at the call whatever the
// caller passed.
BEZEL_TEST(math_minmax_nullable_7660,
           min_nullable_parameter_is_refused_by_7) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(F(2.5)); "
        "    return 0; "
        "  end Main; "
        "  public static method F(d: double?): double "
        "    return Math.Min(d, 1.0); "
        "  end F; "
        "end C;");
    expectRefusedBySection7(r, "Min");
}

// A MUTABLE FIELD never narrows either (§7.4).
BEZEL_TEST(math_minmax_nullable_7660,
           min_nullable_field_is_refused_by_7) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public f: double?; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine(Math.Min(c.f, 1.0)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRefusedBySection7(r, "Min");
}

// ─── THE NEIGHBOUR THAT KEEPS ITS OWN DIAGNOSTIC ──────────────────────────
//
// An un-narrowed `string?` / a nullable RECORD is refused by §14.2's
// CONSTRAINT rule — a different rule, with its own wording — and the §7 gate
// must not add a SECOND diagnostic to that call (one diagnostic per call).
// This row pins BOTH halves: the §14.2 wording is there, and the §7 wording
// is NOT.
BEZEL_TEST(math_minmax_nullable_7660,
           min_nullable_string_keeps_the_14_2_diagnostic) {
    auto r = runAot(prog(
        "    var s: string? := null; "
        "    Console.WriteLine(Math.Min(s, \"a\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "nullable type `string?` does not satisfy the `where T: "
           "IComparable<T>` constraint"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "is nullable but the target type is non-nullable"));
}

// ===========================================================================
// The CONTROLS — the legal neighbours, unchanged.
// ===========================================================================

// The plain numeric selectors still work and still answer by §2.2's order.
BEZEL_TEST(math_minmax_nullable_7660, min_double_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Min(1.0, 2.0));")),
                    "1");
}

BEZEL_TEST(math_minmax_nullable_7660, max_double_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Max(1.0, 2.0));")),
                    "2");
}

BEZEL_TEST(math_minmax_nullable_7660, clamp_double_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Clamp(5.0, 0.0, 1.0));")),
        "1");
}

BEZEL_TEST(math_minmax_nullable_7660, min_int_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Min(3, 4));")),
                    "3");
}

// `string` conforms to `IComparable<string>` (stdlib §9.6), so the string
// form is admitted and answers by the ORDINAL ordering.
BEZEL_TEST(math_minmax_nullable_7660, min_string_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Min(\"b\", \"a\").Length);")),
        "1");
}

// The EXPLICIT TYPE-ARGUMENT spelling of a legal call.
BEZEL_TEST(math_minmax_nullable_7660,
           min_explicit_type_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Min<double>(1.0, 2.0));")),
        "1");
}

// ─── THE §7.4-NARROWED ARGUMENT — the legal neighbour of the refused one ──
//
// §7.4 (v0.355) narrows a local, and §22.6 reads the argument's type AT THE
// CALL, so a narrowed `double?` is a `double` at the call and the call is
// ADMITTED. This is the pair that shows the fix distinguishes the two cases
// rather than refusing every nullable spelling.

BEZEL_TEST(math_minmax_nullable_7660,
           min_declaration_narrowed_double_still_works_7_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d: double? := 2.5; "
            "    Console.WriteLine(Math.Min(d, 1.0));")),
        "1");
}

BEZEL_TEST(math_minmax_nullable_7660,
           min_null_test_narrowed_double_still_works_7_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d: double? := 2.5; "
            "    if d <> null then "
            "      Console.WriteLine(Math.Min(d, 1.0)); "
            "    end if;")),
        "1");
}

BEZEL_TEST(math_minmax_nullable_7660,
           max_null_test_narrowed_double_still_works_7_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d: double? := 2.5; "
            "    if d <> null then "
            "      Console.WriteLine(Math.Max(d, 1.0)); "
            "    end if;")),
        "2.5");
}

BEZEL_TEST(math_minmax_nullable_7660,
           clamp_null_test_narrowed_double_still_works_7_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d: double? := 2.5; "
            "    if d <> null then "
            "      Console.WriteLine(Math.Clamp(d, 0.0, 1.0)); "
            "    end if;")),
        "1");
}

// The §7.8 null-forgiving read is the present value (unchanged).
BEZEL_TEST(math_minmax_nullable_7660,
           min_forced_double_still_works_7_8) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d: double? := 2.5; "
            "    Console.WriteLine(Math.Min(d!, 1.0));")),
        "1");
}

// A narrowed `string?` is a `string` at the call (§7.4, §22.6), and
// `string` conforms to `IComparable<string>` (stdlib §9.6).
BEZEL_TEST(math_minmax_nullable_7660,
           min_null_test_narrowed_string_still_works_7_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s: string? := \"b\"; "
            "    if s <> null then "
            "      Console.WriteLine(Math.Min(s, \"a\").Length); "
            "    end if;")),
        "1");
}

// ─── THE CONTROL THAT IS NOT ABOUT `Math.Min` ──────────────────────────────
//
// The SIBLING `Math.Sign` (`where T: INumber<T>`) already drew the house §7
// wording before this fix (#7227). It must keep drawing it: this fix is the
// same rule applied to the three selectors, not a new rule.
BEZEL_TEST(math_minmax_nullable_7660, sign_nullable_double_is_refused_by_7) {
    auto r = runAot(prog(
        "    var d: double? := null; "
        "    Console.WriteLine(Math.Sign(d));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Math.Sign` is nullable but the target type is "
           "non-nullable (§7)"));
}
