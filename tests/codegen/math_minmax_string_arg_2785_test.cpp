// Regression tests for #2785 — `Math.Min` / `Math.Max` / `Math.Clamp` with a
// STRING argument.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §2.2 ("`Math`", v0.240) declares
//     public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
//     public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
// and stdlib §9.6 ("`String`") declares
//     public class String: IEnumerable<char>, IComparable<string>,
//                          IEquatable<string>
// so `string` satisfies the constraint and `Math.Min("b", "a")` is
// document-admissible: it must compile and answer by §9.6's ORDINAL ordering.
//
// THE DEFECT WAS A COMPILER ABORT.  `comparableCompareTo` admitted only
// Record|Enum|Class (and, after #2782, Interface).  A string argument is none of
// those and has no MethodSymbol to return, so the Min/Max/Clamp arms fell
// through to the FLOAT path and `CreateSIToFP` was handed a pointer:
//     SIToFP source must be integer or integer vector
// MEASURED on `1d7ca54` for both a literal and a variable argument.
//
// The fix reuses the ordinal `strcmp` lowering that `Array<string>.Min()`
// already uses (`tryLowerCollectionInstanceCall`'s `isStrElem` arm), so the two
// forms agree by construction — pinned by min_agrees_with_array_min below
// rather than left as a claim.
//
// ⚠ THE NULLABLE HALF is the subtle part.  §14.2: "nullable types do not
// themselves implement the interfaces of their non-nullable counterparts: a
// `null` value satisfies no interface contract", so an UN-narrowed `string?`
// must be REJECTED — it aborted the compiler the same way before this fix.  But
// §7.4 narrowing deliberately does NOT rewrite the variable's exprType
// (ExprTypecheck_impl.hpp:146), so a `string?` proven non-null by
// `if p <> null then …` still presents as nullable at both layers.  Rejecting on
// the declared type alone therefore rejected LEGAL narrowed code — measured, it
// did, on the first attempt at this fix.  Both layers are now narrowing-aware:
// sema consults `nullNarrowed_`, and codegen accepts the nullable TypeRef while
// `emitStrCmp` fails closed unless both operands lowered to pointers.  The two
// cases below (narrowed accepted / un-narrowed rejected) pin that pair.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// Generated-code ASan.  The new arm reads field 0 of each operand as a `char*`
// and hands both to `strcmp`, so a mis-routed operand (or a null one) surfaces
// as a memory error here rather than as a value coincidence.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Diagnostic TEXT, not the coarse reject: runAot synthesizes an Error for a
// runtime SIGSEGV too, and the defect under test WAS an abort — exactly the
// confusion that would otherwise hide.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

std::string main_(const std::string& body) {
    return "module M; class C  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// #2785 / stdlib §2.2 + §9.6 — the reported repro, which ABORTED the compiler.
// Min("b", "a") is "a", so Length is 1.
BEZEL_TEST(math_minmax_string_arg_2785, min_string_literals) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var s := Math.Min(\"b\", \"a\"); "
        "    return s.Length; ")), 1);
}

// Variables rather than literals, and both directions in one answer:
// Min("pear","fig") = "fig" (3), Max = "pear" (4) → 3*10 + 4 = 34.  Well under
// runAot's 8-bit WEXITSTATUS mask, so no truncation is involved.
BEZEL_TEST(math_minmax_string_arg_2785, min_max_string_variables) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var p: string := \"pear\"; "
        "    var q: string := \"fig\"; "
        "    return Math.Min(p, q).Length * 10 + Math.Max(p, q).Length; ")),
        34);
}

// stdlib §9.6 fixes string ordering as ORDINAL, not case-folded: 'Z' is 90 and
// 'a' is 97, so Min("Z", "a") is "Z".  A case-insensitive or locale ordering
// would answer "a" and this case would catch it.
BEZEL_TEST(math_minmax_string_arg_2785, ordinal_ordering_not_case_folded) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var p: string := \"Z\"; "
        "    var q: string := \"a\"; "
        "    var m := Math.Min(p, q); "
        "    if m = \"Z\" then return 90; end if; "
        "    return 97; ")), 90);
}

// The fix reuses the SAME ordinal lowering `Array<string>.Min()` uses.  This
// pins the agreement rather than asserting it in a comment: if the two forms
// ever diverge, this case fails.
//
// Plain runAot, not runChecked, for the INDEPENDENT pre-existing `Array<string>`
// collection-literal leak recorded on #2774 (176 bytes / 2 allocs, independent
// of any collection operation) — LeakSanitizer overrides the process exit code
// and would mask the value under test.  Same accommodation, for the same shape
// and the same reason, as control_string_element_still_ordinal in the #2770
// test.  Nothing is lost here: this case exists to pin ORDERING AGREEMENT, and
// the memory-error half of the oracle is carried by the eight runChecked cases
// around it, which reach the identical strcmp arm.
BEZEL_TEST(math_minmax_string_arg_2785, min_agrees_with_array_min) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var p: string := \"pear\"; "
        "    var q: string := \"fig\"; "
        "    var arr: Array<string> := [p, q]; "
        "    var viaMath := Math.Min(p, q); "
        "    var viaArr := arr.Min(); "
        "    if viaMath = viaArr then return 11; end if; "
        "    return 22; ")), 11);
}

// `Clamp` reaches the same comparator from two further call sites.  "a" is below
// the low bound "b", so it clamps up to "b".
BEZEL_TEST(math_minmax_string_arg_2785, clamp_string_below_low_bound) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var lo: string := \"b\"; "
        "    var v: string := \"a\"; "
        "    var hi: string := \"y\"; "
        "    var r := Math.Clamp(v, lo, hi); "
        "    if r = \"b\" then return 61; end if; "
        "    return 62; ")), 61);
}

// ...and a value inside the range is returned unchanged.
BEZEL_TEST(math_minmax_string_arg_2785, clamp_string_within_range) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var lo: string := \"b\"; "
        "    var v: string := \"m\"; "
        "    var hi: string := \"y\"; "
        "    var r := Math.Clamp(v, lo, hi); "
        "    if r = \"m\" then return 63; end if; "
        "    return 64; ")), 63);
}

// EQUAL operands — strcmp returns 0, so Min takes the `<= 0` branch and Max the
// `>= 0` branch; both must still yield a valid string (4*10 + 4).
BEZEL_TEST(math_minmax_string_arg_2785, equal_strings_both_branches) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var p: string := \"same\"; "
        "    var q: string := \"same\"; "
        "    return Math.Min(p, q).Length * 10 + Math.Max(p, q).Length; ")),
        44);
}

// The EMPTY string — its data pointer must still be a valid C string for
// strcmp.  Min is "" (0) and Max is "x" (1) → 0*10 + 1.
BEZEL_TEST(math_minmax_string_arg_2785, empty_string_operand) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var p: string := \"\"; "
        "    var q: string := \"x\"; "
        "    return Math.Min(p, q).Length * 10 + Math.Max(p, q).Length; ")),
        1);
}

// §7.4 NARROWED `string?` — legal, and must NOT be rejected.  The first attempt
// at this fix rejected exactly this shape, because narrowing does not rewrite
// the exprType.  Value-checked (34, not merely "it compiled"), which is also
// what establishes that `string?` shares `string`'s plain-pointer
// representation rather than a tagged one.
BEZEL_TEST(math_minmax_string_arg_2785, narrowed_nullable_string_accepted) {
    BEZEL_EXPECT_EQ(runChecked(main_(
        "    var p: string? := \"pear\"; "
        "    var q: string := \"fig\"; "
        "    if p <> null then "
        "      return Math.Min(p, q).Length * 10 + Math.Max(p, q).Length; "
        "    end if; "
        "    return 99; ")), 34);
}

// ...and an UN-narrowed `string?` is rejected per §14.2, rather than aborting
// the compiler as it did before this fix.
BEZEL_TEST(math_minmax_string_arg_2785, unnarrowed_nullable_string_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(main_(
        "    var p: string? := null; "
        "    var q: string := \"x\"; "
        "    return Math.Min(p, q).Length; "),
        "does not satisfy the `where T: IComparable<T>` constraint"));
}

// CONTROL — the integer path, which never consults the string arm.  Plain
// runAot: nothing is allocated.
BEZEL_TEST(math_minmax_string_arg_2785, control_int_argument_unchanged) {
    BEZEL_EXPECT_EQ(run(main_(
        "    return Math.Min(7, 3) * 10 + Math.Max(7, 3); ")), 37);
}

// CONTROL — the double path, likewise untouched (the string arm is tried only
// after the integer and CompareTo arms, and before the float fallback).
BEZEL_TEST(math_minmax_string_arg_2785, control_double_argument_unchanged) {
    BEZEL_EXPECT_EQ(run(main_(
        "    var a: double := 2.5; "
        "    var b: double := 1.5; "
        "    var m := Math.Min(a, b); "
        "    if m < 2.0 then return 15; end if; "
        "    return 25; ")), 15);
}
