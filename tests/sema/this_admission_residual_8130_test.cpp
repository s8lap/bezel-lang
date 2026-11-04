// Regression tests for #8130 (spec §12.3, §5.1, §22.6, §22.8, §26.1,
// v0.355; stdlib §2.2, §2.3, §9.6): the RESIDUAL of #7304 — the `this`
// spelling was still admitted at three MORE built-in gates.
//
// ─── The normative sentences ───────────────────────────────────────────────
//
// spec §12.3 (v0.355) — `this` names the ENCLOSING TYPE; `base` names the
// BASE CLASS. §9.4 gives a value kind's member body the same reading
// ("`this` binds a borrow of the receiver value"), and §26.1 gives an
// extension block's receiver the same reading ("inside an extension block,
// `this` is the receiver").
//
// spec §5.1 (v0.355) — the implicit-conversion table admits NO conversion
// from a user type to `string`, and none from a user type to a numeric type.
// The reference conversions below the table admit a class to `Object` and to
// a base class; `string` and the numeric types are in neither.
//
// spec §22.6 (v0.355) — "If no candidate is applicable at all — no candidate
// is reachable: a conversion §5.1 does not admit as implicit, … the call is a
// compile error with no candidates cited, because no overload was reachable."
//
// stdlib §9.6 (v0.355) declares `StartsWith(value: string): bool`; stdlib
// §2.2 declares `Abs(x: int): int` / `Abs(x: long): long` /
// `Abs(x: double): double` / `Abs(x: decimal): decimal`; stdlib §2.3
// declares `ToInt32(value: string): int`.
//
// ─── The defect ────────────────────────────────────────────────────────────
//
// #7304 (b286aed1) closed the `this` / `base` hole at the VALUE built-ins'
// admission gate (`ExprTC::checkBuiltinCallAdmission`), by RECOVERING the
// receiver's static type where that gate asks §5.1's question. It did NOT
// reach the OTHER two gates that ask the same question of the same
// arguments:
//
//   (a) the §9.6 STRING-RECEIVER gate (`ExprTC::checkBuiltinStringDeclaredParams`),
//   (b) the §2.2 / §2.3 `Math` / `Convert` gate (`ExprTC::checkBuiltinMathConvertCallArgs`).
//
// Both read the argument's STATIC TYPE, and the resolver deliberately leaves
// the `this` / `base` spellings UNRESOLVED ("Slice 3b: IdentifierExpr name
// lookup. `this` / `base` are implicit-meaning identifiers — leave them
// unresolved" — src/sema/ExprTypecheck_Exprs.cpp), so `ctx_.exprType` of
// either is null in every ordinary body. With the type missing the position
// had NO opinion and the gate STOOD DOWN, so:
//
//   "abc".StartsWith(this)   -> compiled clean and ran; the receiver's OWN
//                                BYTES reached the callee as a `bz.string`
//                                descriptor;
//   Convert.ToInt32(this)    -> compiled clean and threw at run time
//                                (exit 70) — the receiver's own bytes read
//                                as text;
//   Math.Abs(this)           -> the compiler ABORTED inside the LLVM
//                                verifier ("Both operands to ICmp
//                                instruction are not of the same type!",
//                                "Integer arithmetic operators only work
//                                with integral types!") — an ICE in the
//                                COMPILER, on a program that should have
//                                been refused.
//
// MEASURED on the base arm (the staging tip this lane started from), with
// the LOCAL-INSTANCE twin of each call as the reference:
//
//   "abc".StartsWith(this)   vs  "abc".StartsWith(o)   [o := new C()]
//   Convert.ToInt32(this)    vs  Convert.ToInt32(o)
//   Math.Abs(this)           vs  Math.Abs(o)
//
// where the local twin is REFUSED with the §22.6 diagnostic and the `this`
// spelling is admitted (or ICEs).
//
// ─── The fix ───────────────────────────────────────────────────────────────
//
// The receiver's type is RECOVERED at those two gates as well — §12.3's own
// reading of it — and §5.1's question is asked of the recovered type, so the
// call is refused with the SAME diagnostic the local-instance spelling
// draws, at the call, before codegen ever sees it.
//
// This is a RECOVERY, not a re-typing: the resolver's "leave them
// unresolved" deferral stands, `ctx_.exprType` is unchanged, and the recovered
// type is read at those two gates ONLY.
//
// ─── What the rows assert ──────────────────────────────────────────────────
//
// The load-bearing observable is the REFUSAL: the FULL diagnostic text, at a
// real location (`failedAtCompileTimeWithRealLocation`), never the absence
// of a diagnostic. The CONTROL rows pin the other side of the boundary —
// the local-instance twin of each call (refused on the base arm too), and
// the ADMITTING rows, where the receiver IS of the declared type (`this`
// inside `extension string` / `extension int` / `extension double`, §26.1).
// The admitting rows are asserted by their RESULT, so a row cannot pass on a
// compile that merely did not fail.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The refusal oracle: the FULL diagnostic text, at a real location, and the
// compiler did not merely abort.
void expectRefused(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The program under test: the receiver type's declaration, and a `Main` that
// runs the receiver's method.
std::string prog(const std::string& decls) {
    return "module M;\n" + decls +
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var c := new C();\n"
           "    return c.Go();\n"
           "  end Main;\n"
           "end Program;\n";
}

// A CLASS receiver (§12.3: `this` is the enclosing class).
std::string classDecls(const std::string& body) {
    return "public class C\n"
           "  public method Go(): int\n" + body +
           "    return 0;\n"
           "  end Go;\n"
           "end C;\n";
}

// A DERIVED class (§12.3: `base` is the base class).
std::string derivedDecls(const std::string& body) {
    return "public class B\n"
           "  public F: int := 3;\n"
           "end B;\n"
           "public class C : B\n"
           "  public method Go(): int\n" + body +
           "    return 0;\n"
           "  end Go;\n"
           "end C;\n";
}

// A RECORD receiver (§9.4: a value kind's member body binds `this` to the
// receiver value).
std::string recordDecls(const std::string& body) {
    return "public record C\n"
           "  public F: int := 3;\n"
           "  public method Go(): int\n" + body +
           "    return 0;\n"
           "  end Go;\n"
           "end C;\n";
}

// The §9.6 string-receiver gate's diagnostic, in full. `argType` is the
// argument's OWN type as the diagnostic spells it (§5.1's rule has no
// conversion from it).
const char* const kStartsWithDiag =
    "no matching overload of `string.StartsWith` \xE2\x80\x94 stdlib "
    "\xC2\xA7""9.6 declares `StartsWith(value: string): bool`, so the `value` "
    "argument must be a `string` \xE2\x80\x94 the argument has type `C`, "
    "which is never implicitly convertible to it (\xC2\xA7""5.1)";

// The §2.2 `Math` gate's diagnostic, in full.
const char* const kMathAbsDiag =
    "no matching overload of `Math.Abs` \xE2\x80\x94 stdlib \xC2\xA7""2.2 "
    "declares `Abs(x: int): int` / `Abs(x: long): long` / "
    "`Abs(x: double): double` / `Abs(x: decimal): decimal`, so the `x` "
    "argument must be an `int`, a `long`, a `double` or a `decimal` "
    "(stdlib \xC2\xA7""2.2; \xC2\xA7""5.1)";

// The §2.3 `Convert` gate's diagnostic, in full.
const char* const kConvertToInt32Diag =
    "no matching overload of `Convert.ToInt32` \xE2\x80\x94 stdlib "
    "\xC2\xA7""2.3 declares `ToInt32(value: string): int`, so the `value` "
    "argument must be a `string` (stdlib \xC2\xA7""2.3; \xC2\xA7""5.1)";

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE §9.6 STRING-RECEIVER GATE — `"abc".StartsWith(this)`.
// ---------------------------------------------------------------------------

// `this` is the enclosing class (§12.3), and §5.1 admits no conversion from
// a class to `string`. The call must be refused, with the diagnostic the
// local-instance spelling draws.
BEZEL_TEST(this_admission_residual_8130,
           string_startswith_this_in_a_class_is_refused) {
    JitResult r = runAot(prog(classDecls(
        "    var b: bool := \"abc\".StartsWith(this);\n")));
    expectRefused(r, kStartsWithDiag);
}

// The same call in a DERIVED class — `this` is still the enclosing class
// (§12.3), so the same refusal.
BEZEL_TEST(this_admission_residual_8130,
           string_startswith_this_in_a_derived_class_is_refused) {
    JitResult r = runAot(prog(derivedDecls(
        "    var b: bool := \"abc\".StartsWith(this);\n")));
    expectRefused(r, kStartsWithDiag);
}

// The same call in a RECORD — §9.4 binds `this` to the receiver VALUE, and
// a record is not a `string` either (§5.1).
BEZEL_TEST(this_admission_residual_8130,
           string_startswith_this_in_a_record_is_refused) {
    JitResult r = runAot(prog(recordDecls(
        "    var b: bool := \"abc\".StartsWith(this);\n")));
    expectRefused(r, kStartsWithDiag);
}

// THE LOCAL-INSTANCE TWIN: the same call with a local `C` at the same
// position. #7290's refusal stands, with the same diagnostic — the recovery
// did not change it. (Green on the base arm: the POSITIVE CONTROL that
// shows the shape is genuinely refused for the type, not for an unrelated
// reason.)
BEZEL_TEST(this_admission_residual_8130,
           string_startswith_a_local_class_instance_is_refused) {
    JitResult r = runAot(prog(classDecls(
        "    var o := new C();\n"
        "    var b: bool := \"abc\".StartsWith(o);\n")));
    expectRefused(r, kStartsWithDiag);
}

// THE ADMITTING ROW: inside `extension string`, `this` IS the receiver and
// the receiver IS a `string` (§26.1) — so the call is legal, and it is
// asserted by its RESULT.
BEZEL_TEST(this_admission_residual_8130,
           string_startswith_this_inside_extension_string_is_admitted) {
    JitResult r = runAot(
        "module M;\n"
        "extension string\n"
        "  public method StartsWithMe(): bool\n"
        "    return \"abc\".StartsWith(this);\n"
        "  end StartsWithMe;\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"abc\";\n"
        "    Console.WriteLine(s.StartsWithMe());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ---------------------------------------------------------------------------
// (b) THE §2.3 `Convert` GATE — `Convert.ToInt32(this)`.
// ---------------------------------------------------------------------------

// stdlib §2.3 declares `ToInt32(value: string): int`; §5.1 admits no
// conversion from a class to `string`.
BEZEL_TEST(this_admission_residual_8130,
           convert_toint32_this_in_a_class_is_refused) {
    JitResult r = runAot(prog(classDecls(
        "    var n: int := Convert.ToInt32(this);\n")));
    expectRefused(r, kConvertToInt32Diag);
}

// The same call in a DERIVED class.
BEZEL_TEST(this_admission_residual_8130,
           convert_toint32_this_in_a_derived_class_is_refused) {
    JitResult r = runAot(prog(derivedDecls(
        "    var n: int := Convert.ToInt32(this);\n")));
    expectRefused(r, kConvertToInt32Diag);
}

// THE LOCAL-INSTANCE TWIN — the positive control, green on the base arm.
BEZEL_TEST(this_admission_residual_8130,
           convert_toint32_a_local_class_instance_is_refused) {
    JitResult r = runAot(prog(classDecls(
        "    var o := new C();\n"
        "    var n: int := Convert.ToInt32(o);\n")));
    expectRefused(r, kConvertToInt32Diag);
}

// THE ADMITTING ROW: inside `extension string`, `this` IS a `string`
// (§26.1), so `Convert.ToInt32(this)` is legal — and it parses the text.
BEZEL_TEST(this_admission_residual_8130,
           convert_toint32_this_inside_extension_string_is_admitted) {
    JitResult r = runAot(
        "module M;\n"
        "extension string\n"
        "  public method ToInt32Me(): int\n"
        "    return Convert.ToInt32(this);\n"
        "  end ToInt32Me;\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"42\";\n"
        "    Console.WriteLine(s.ToInt32Me());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// ---------------------------------------------------------------------------
// (c) THE §2.2 `Math` GATE — `Math.Abs(this)` (the ICE).
// ---------------------------------------------------------------------------

// stdlib §2.2 declares `Abs` over the four numeric types; §5.1 admits no
// conversion from a class to any of them. On the base arm this call was not
// refused at all — it reached codegen and the compiler ABORTED inside the
// LLVM verifier.
BEZEL_TEST(this_admission_residual_8130,
           math_abs_this_in_a_class_is_refused) {
    JitResult r = runAot(prog(classDecls(
        "    var n: int := Math.Abs(this);\n")));
    expectRefused(r, kMathAbsDiag);
}

// The same call in a DERIVED class.
BEZEL_TEST(this_admission_residual_8130,
           math_abs_this_in_a_derived_class_is_refused) {
    JitResult r = runAot(prog(derivedDecls(
        "    var n: int := Math.Abs(this);\n")));
    expectRefused(r, kMathAbsDiag);
}

// THE LOCAL-INSTANCE TWIN — the positive control, green on the base arm.
BEZEL_TEST(this_admission_residual_8130,
           math_abs_a_local_class_instance_is_refused) {
    JitResult r = runAot(prog(classDecls(
        "    var o := new C();\n"
        "    var n: int := Math.Abs(o);\n")));
    expectRefused(r, kMathAbsDiag);
}

// THE ADMITTING ROW: inside `extension int`, `this` IS an `int` (§26.1), so
// `Math.Abs(this)` is legal — and it is the integer overload's answer.
BEZEL_TEST(this_admission_residual_8130,
           math_abs_this_inside_extension_int_is_admitted) {
    JitResult r = runAot(
        "module M;\n"
        "extension int\n"
        "  public method AbsMe(): int\n"
        "    return Math.Abs(this);\n"
        "  end AbsMe;\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var i := -5;\n"
        "    Console.WriteLine(i.AbsMe());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// THE ADMITTING ROW for the other numeric overload: inside `extension
// double`, `this` IS a `double` (§26.1).
BEZEL_TEST(this_admission_residual_8130,
           math_abs_this_inside_extension_double_is_admitted) {
    JitResult r = runAot(
        "module M;\n"
        "extension double\n"
        "  public method AbsMe(): double\n"
        "    return Math.Abs(this);\n"
        "  end AbsMe;\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var d := -5.5;\n"
        "    Console.WriteLine(d.AbsMe());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5.5") != std::string::npos);
}

// ---------------------------------------------------------------------------
// THE OTHER SPELLING #7304 LEFT OPEN: `base` (§12.3).
// ---------------------------------------------------------------------------

// `base` names the BASE CLASS (§12.3), so it is a `B` and not a `string`
// (§5.1) — the same refusal.
BEZEL_TEST(this_admission_residual_8130,
           string_startswith_base_in_a_derived_class_is_refused) {
    JitResult r = runAot(prog(derivedDecls(
        "    var b: bool := \"abc\".StartsWith(base);\n")));
    expectRefused(r,
                   "no matching overload of `string.StartsWith` \xE2\x80\x94 "
                   "stdlib \xC2\xA7""9.6 declares "
                   "`StartsWith(value: string): bool`, so the `value` argument "
                   "must be a `string` \xE2\x80\x94 the argument has type `B`, "
                   "which is never implicitly convertible to it "
                   "(\xC2\xA7""5.1)");
}

// The `Math` gate's twin of the same spelling.
BEZEL_TEST(this_admission_residual_8130,
           math_abs_base_in_a_derived_class_is_refused) {
    JitResult r = runAot(prog(derivedDecls(
        "    var n: int := Math.Abs(base);\n")));
    expectRefused(r, kMathAbsDiag);
}

// The `Convert` gate's twin of the same spelling.
BEZEL_TEST(this_admission_residual_8130,
           convert_toint32_base_in_a_derived_class_is_refused) {
    JitResult r = runAot(prog(derivedDecls(
        "    var n: int := Convert.ToInt32(base);\n")));
    expectRefused(r, kConvertToInt32Diag);
}

// ---------------------------------------------------------------------------
// ONE DIAGNOSTIC PER CALL: the §12.3 / §22.8 SPELLING errors stand alone.
// ---------------------------------------------------------------------------

// §12.3: "`this` is not available in a static method or static constructor".
// The compiler already refuses the SPELLING there, so the recovery must not
// add a SECOND diagnostic to the same call: the §22.6 refusal stands down
// and the §12.3 refusal is the one that is reported.
BEZEL_TEST(this_admission_residual_8130,
           math_abs_this_in_a_static_method_reports_only_the_spelling_error) {
    JitResult r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Go(): int\n"
        "    var n: int := Math.Abs(this);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRefused(r,
                   "`this` is not available in a static method or static "
                   "constructor");
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, "no matching overload of "
                                             "`Math.Abs`"),
                     0);
}
