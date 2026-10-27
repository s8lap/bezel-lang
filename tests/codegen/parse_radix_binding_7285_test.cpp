// Regression tests for #7285 (spec §5.1 / §22.6 / §17.14, v0.344; stdlib
// §9.1, v0.344): the integer `Parse(s, radix)` / `TryParse(s, radix, out)`
// radix argument was never checked against the declared `radix: int`
// parameter, and the lowering truncated it to i32 whatever its type.
//
// ─── The normative sentences ───────────────────────────────────────────────
// stdlib §9.1 (v0.344) declares the radix forms:
//
//   public static pure method Parse(s: string, radix: int): int;
//   public static method TryParse(s: string, radix: int, out result: int): bool;
//
// spec §22.6 (v0.344): "If no candidate is applicable at all — no candidate
// is reachable: a conversion §5.1 does not admit as implicit … the call is a
// compile error with no candidates cited."
//
// spec §5.1 (v0.344) says which arguments reach an `int` parameter: the
// IDENTITY (`int`), the widening table (`sbyte`, `byte`, `short`, `ushort`,
// `char` → `int`), and §5.1's CONSTANT NARROWING — "the source may be a
// compile-time constant of any integer type — not only `int` — and the target
// any integer type whose range contains the value, across signedness (… a
// `uint`-typed constant `7` is admissible where `int` is expected)".
//
// ─── The defect ────────────────────────────────────────────────────────────
// The lowering was `if (!radix->getType()->isIntegerTy(32)) radix =
// builder_.CreateTrunc(radix, i32());` — a TRUNCATION of whatever the
// argument was:
//
//   * a `bool` (i1) radix → `trunc i1 to i32`, a trunc to a WIDER type:
//     malformed IR, and the compiler ABORTED with "IR verification failed
//     (malformed IR emitted)";
//   * a `byte`/`sbyte` (i8) or `short`/`ushort` (i16) radix → `trunc i8 to
//     i32`: the SAME abort — for an argument §5.1 ADMITS;
//   * a `double`/`float`/`decimal`/`string` radix → `trunc double to i32`:
//     the same abort;
//   * a NULLABLE radix → `trunc {i8, i32} to i32`: the same abort;
//   * a runtime `uint`/`ulong`/`long` radix → silently accepted (a `long`
//     radix silently truncated to 32 bits).
//
// And the 3-argument `TryParse(s, radix, out result)` form was NOT lowered
// at all: the call was dropped and the `out` slot kept its zero-initialized
// value, so `int.TryParse("ff", 16, out v)` returned false with v = 0 for
// EVERY input.
//
// ─── The mirror ────────────────────────────────────────────────────────────
// stdlib §2.3's `Convert.ToString(value, toBase)` is the house precedent for
// the radix rule (its check lives in sema): a `byte`/`sbyte`/`short`/
// `ushort`/`char` radix BINDS, a `uint`/`ulong`/`long` CONSTANT radix binds
// by §5.1's constant narrowing, and everything else is refused with "no
// matching overload of `Convert.ToString`". The rows below pin that the two
// sites now AGREE.
//
// The rows that were green on the base arm are CONTROLS: the `int` radix,
// the one-argument `Parse`, the two-argument `TryParse`, and the constant
// radices (which the old truncation happened to compute correctly).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The house diagnostic: the argument, the type stdlib §9.1 declares there,
// and the §5.1 rule that decides the verdict.
const char* kRadixNeedle = "so the `radix` argument must be an `int`";

// A `Parse(s, radix)` program whose radix argument is `radix` — a literal
// when `decl` is empty, otherwise a runtime local of the declared type.
std::string parseProg(const std::string& decl, const std::string& radix) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" + decl +
           "    Console.WriteLine(int.Parse(\"ff\", " + radix + "));\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A `TryParse(s, radix, out result)` program.
std::string tryParseProg(const std::string& decl, const std::string& radix) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" + decl +
           "    var v: int := 0;\n"
           "    var ok: bool := int.TryParse(\"ff\", " + radix + ", out v);\n"
           "    Console.WriteLine(ok);\n"
           "    Console.WriteLine(v);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A `TryParse(s, radix, out result)` program whose FIRST argument is `first`
// — the operand stdlib §9.1 declares as `s: string`.
std::string tryParseFirstArgProg(const std::string& first,
                                  const std::string& radix) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var v: int := 0;\n"
           "    var ok: bool := int.TryParse(" + first + ", " + radix +
           ", out v);\n"
           "    Console.WriteLine(ok);\n"
           "    Console.WriteLine(v);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A `TryParse(s, out result)` program (the two-argument form) whose FIRST
// argument is `first`.
std::string tryParseTwoArgFirstArgProg(const std::string& first) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var v: int := 0;\n"
           "    var ok: bool := int.TryParse(" + first + ", out v);\n"
           "    Console.WriteLine(ok);\n"
           "    Console.WriteLine(v);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A `Parse(s, radix)` program whose FIRST argument is `first` — the operand
// stdlib §9.1 declares as `s: string`.
std::string parseFirstArgProg(const std::string& first,
                               const std::string& radix) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    Console.WriteLine(int.Parse(" + first + ", " + radix + "));\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The program's stdout — the parsed value, byte-for-byte.
std::string parseOut(const std::string& decl, const std::string& radix) {
    return runAot(parseProg(decl, radix)).stdoutText;
}

void expectRejected(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

// THE COMPILER'S OUTCOME for a program the first-argument guard covers.
//
// The load-bearing assertion here is `r.ran`: the program must COMPILE, link
// and RUN. The arm that lowered a non-string operand into a string operand
// ABORTED the compiler ("IR verification failed (malformed IR emitted)"), so
// there `ran` is false and this row is RED. The program's OUTPUT is NOT the
// discriminator — the dropped call prints "False"/"0" on the aborting arm's
// predecessor too — so the output assertion below is a CONTROL, not the pin.
void expectCompiledWithoutIRFailure(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "codegen error"));
}

}  // namespace

// ---------------------------------------------------------------------------
// The REFUSED forms: a radix that binds to no declared `int` parameter.
// ---------------------------------------------------------------------------

// A `bool` radix is never implicitly convertible to an `int` (§5.1: "Between
// integers and `bool`" is never implicit). On the base arm the COMPILER
// ABORTED with malformed IR (`trunc i1 to i32`).
BEZEL_TEST(parse_radix_binding_7285, bool_literal_radix_is_rejected) {
    JitResult r = runAot(parseProg("", "true"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `bool` is never implicitly convertible to an `int`"));
}

// The same for a runtime `bool` — the shape that aborted the compiler.
BEZEL_TEST(parse_radix_binding_7285, bool_runtime_radix_is_rejected) {
    JitResult r = runAot(parseProg(
        "    var radix: bool := true;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `bool` is never implicitly convertible to an `int`"));
}

// A `string` radix binds to no declared parameter either.
BEZEL_TEST(parse_radix_binding_7285, string_literal_radix_is_rejected) {
    JitResult r = runAot(parseProg("", "\"16\""));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `string` is never implicitly convertible to an `int`"));
}

BEZEL_TEST(parse_radix_binding_7285, string_runtime_radix_is_rejected) {
    JitResult r = runAot(parseProg(
        "    var radix: string := \"16\";\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `string` is never implicitly convertible to an `int`"));
}

// A runtime `uint` is not a compile-time constant, so §5.1's constant
// narrowing does not reach it and §5.1's never-implicit list governs
// ("Between signed and unsigned types of the same or narrower width … is
// never implicit"). On the base arm it was SILENTLY ACCEPTED.
BEZEL_TEST(parse_radix_binding_7285, runtime_uint_radix_is_rejected) {
    JitResult r = runAot(parseProg(
        "    var radix: uint := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `uint` reaches an `int` parameter only as a compile-time constant"));
}

// The same for `ulong` and `long` — both reach an `int` parameter only by
// §5.1's constant narrowing.
BEZEL_TEST(parse_radix_binding_7285, runtime_ulong_radix_is_rejected) {
    JitResult r = runAot(parseProg(
        "    var radix: ulong := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `ulong` reaches an `int` parameter only as a compile-time constant"));
}

BEZEL_TEST(parse_radix_binding_7285, runtime_long_radix_is_rejected) {
    JitResult r = runAot(parseProg(
        "    var radix: long := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `long` reaches an `int` parameter only as a compile-time constant"));
}

// A `double`, a `float` and a `decimal` bind to no `int` parameter. On the
// base arm each of them ABORTED the compiler (`trunc double to i32`).
BEZEL_TEST(parse_radix_binding_7285, double_radix_is_rejected) {
    JitResult r = runAot(parseProg("", "16.0"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `double` is never implicitly convertible to an `int`"));
}

BEZEL_TEST(parse_radix_binding_7285, float_radix_is_rejected) {
    JitResult r = runAot(parseProg("", "16.0f"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `float` is never implicitly convertible to an `int`"));
}

BEZEL_TEST(parse_radix_binding_7285, decimal_radix_is_rejected) {
    JitResult r = runAot(parseProg("", "16.0m"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `decimal` is never implicitly convertible to an `int`"));
}

// A NULLABLE radix reaches no non-nullable `int` parameter: §5.1's nullable
// lifting converts a `T` to a `T?`, never the other way. On the base arm the
// nullable COMPANION was truncated (`trunc {i8, i32} to i32`) and the
// compiler aborted.
BEZEL_TEST(parse_radix_binding_7285, nullable_int_radix_is_rejected) {
    JitResult r = runAot(parseProg(
        "    var radix: int? := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `int?` is never implicitly convertible to an `int`"));
}

// A constant whose value does not fit `int`'s range is a COMPILE ERROR at
// the conversion site (§5.1) — never a silent truncation. On the base arm
// `4000000000UL` was truncated to 1410065408 and used as the base.
BEZEL_TEST(parse_radix_binding_7285, out_of_range_constant_radix_is_rejected) {
    JitResult r = runAot(parseProg("", "4000000000UL"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the compile-time constant's value does not fit `int`'s range"));
}

// ---------------------------------------------------------------------------
// The ADMITTED forms §5.1's WIDENING table reaches: a `byte`, `sbyte`,
// `short`, `ushort` or `char` radix binds to the declared `int` parameter —
// and on the base arm each of them ABORTED the compiler (`trunc i8 to i32`).
// ---------------------------------------------------------------------------

BEZEL_TEST(parse_radix_binding_7285, byte_radix_is_admitted_and_parses) {
    BEZEL_EXPECT_EQ(parseOut("    var radix: byte := byte(16);\n", "radix"),
                    std::string("255\n"));
}

BEZEL_TEST(parse_radix_binding_7285, sbyte_radix_is_admitted_and_parses) {
    BEZEL_EXPECT_EQ(parseOut("    var radix: sbyte := sbyte(16);\n", "radix"),
                    std::string("255\n"));
}

BEZEL_TEST(parse_radix_binding_7285, short_radix_is_admitted_and_parses) {
    BEZEL_EXPECT_EQ(parseOut("    var radix: short := short(16);\n", "radix"),
                    std::string("255\n"));
}

BEZEL_TEST(parse_radix_binding_7285, ushort_radix_is_admitted_and_parses) {
    BEZEL_EXPECT_EQ(parseOut("    var radix: ushort := ushort(16);\n", "radix"),
                    std::string("255\n"));
}

// ---------------------------------------------------------------------------
// The CONTROLS — green on BOTH arms.
// ---------------------------------------------------------------------------

// The `int` radix still parses hex.
BEZEL_TEST(parse_radix_binding_7285, int_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("", "16"), std::string("255\n"));
}

// The constant-narrowed radices (a §17.14 constant of ANY integer type whose
// value fits `int`) still parse hex — the base arm computed them correctly
// by accident (an i32/i64 value truncated to i32), and the fix keeps them.
BEZEL_TEST(parse_radix_binding_7285, uint_literal_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("", "16u"), std::string("255\n"));
}

BEZEL_TEST(parse_radix_binding_7285, ulong_literal_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("", "16UL"), std::string("255\n"));
}

BEZEL_TEST(parse_radix_binding_7285, long_literal_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("", "16L"), std::string("255\n"));
}

// The same for a `const` reference (a §17.14 constant).
BEZEL_TEST(parse_radix_binding_7285, uint_const_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("    const K: uint := 16u;\n", "K"),
                    std::string("255\n"));
}

BEZEL_TEST(parse_radix_binding_7285, ulong_const_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("    const K: ulong := 16UL;\n", "K"),
                    std::string("255\n"));
}

BEZEL_TEST(parse_radix_binding_7285, long_const_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("    const K: long := 16L;\n", "K"),
                    std::string("255\n"));
}

// A `char` radix binds by §5.1's widening table (a `char` is already an
// i32 in the lowering, so this one was never truncated).
BEZEL_TEST(parse_radix_binding_7285, char_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("    var radix: char := char(16);\n", "radix"),
                    std::string("255\n"));
}

// The NAMED spelling of the radix behaves like the positional one.
BEZEL_TEST(parse_radix_binding_7285, named_uint_literal_radix_still_parses_hex) {
    BEZEL_EXPECT_EQ(parseOut("", "radix := 16u"), std::string("255\n"));
}

// A CLASS-LEVEL `const` (stdlib §22.12) is a §17.14 constant too — its
// value reaches the radix through `ConstMemberSymbol::intValue`.
BEZEL_TEST(parse_radix_binding_7285, class_const_radix_still_parses_hex) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static const K: uint := 16u;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(int.Parse(\"ff\", C.K));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAot(prog).stdoutText, std::string("255\n"));
}

// The one-argument `Parse(s)` form is untouched.
BEZEL_TEST(parse_radix_binding_7285, one_argument_parse_is_unchanged) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(int.Parse(\"255\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAot(prog).stdoutText, std::string("255\n"));
}

// The two-argument `TryParse(s, out result)` form is untouched.
BEZEL_TEST(parse_radix_binding_7285, two_argument_tryparse_is_unchanged) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: int := 0;\n"
        "    var ok: bool := int.TryParse(\"255\", out v);\n"
        "    Console.WriteLine(ok);\n"
        "    Console.WriteLine(v);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAot(prog).stdoutText, std::string("True\n255\n"));
}

// ---------------------------------------------------------------------------
// The `TryParse(s, radix, out result)` form (stdlib §9.1) — it was NOT
// lowered at all on the base arm: the call was dropped, so it returned false
// with the out slot at its zero-initialized value for EVERY input.
// ---------------------------------------------------------------------------

BEZEL_TEST(parse_radix_binding_7285, tryparse_radix_form_parses_hex) {
    BEZEL_EXPECT_EQ(runAot(tryParseProg("", "16")).stdoutText,
                    std::string("True\n255\n"));
}

// A `TryParse` that fails returns false and the out slot gets the default.
BEZEL_TEST(parse_radix_binding_7285, tryparse_radix_form_returns_false_on_bad_input) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: int := 0;\n"
        "    var ok: bool := int.TryParse(\"zz\", 16, out v);\n"
        "    Console.WriteLine(ok);\n"
        "    Console.WriteLine(v);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAot(prog).stdoutText, std::string("False\n0\n"));
}

// The radix argument of the `TryParse` form is bound by §5.1 exactly as at
// the `Parse(s, radix)` site.
BEZEL_TEST(parse_radix_binding_7285, tryparse_radix_form_rejects_a_bool_radix) {
    JitResult r = runAot(tryParseProg("", "true"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `bool` is never implicitly convertible to an `int`"));
}

BEZEL_TEST(parse_radix_binding_7285, tryparse_radix_form_rejects_a_runtime_uint_radix) {
    JitResult r = runAot(tryParseProg(
        "    var radix: uint := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `uint` reaches an `int` parameter only as a compile-time constant"));
}

// ---------------------------------------------------------------------------
// The MIRROR SITE (stdlib §2.3's `Convert.ToString(value, toBase)`) — the
// house precedent for the radix rule. It lives in sema and is unchanged by
// this fix; the rows below pin that the two sites now agree.
// ---------------------------------------------------------------------------

// The mirror refuses a `bool` radix with its own wording.
BEZEL_TEST(parse_radix_binding_7285, mirror_site_refuses_a_bool_radix) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Convert.ToString(255, true));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
}

// The mirror admits a `uint` CONSTANT radix — the same rule the `Parse`
// site now applies.
BEZEL_TEST(parse_radix_binding_7285, mirror_site_admits_a_uint_constant_radix) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Convert.ToString(255, 16u));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAot(prog).stdoutText, std::string("ff\n"));
}

// ---------------------------------------------------------------------------
// THE FIRST ARGUMENT — stdlib §9.1's `s: string`.
//
// stdlib §9.1 (v0.344) declares `TryParse(s: string, radix: int, out
// result: int): bool` — the first argument is a `string`, and stdlib §9.1
// declares NO span overload of the radix form.
//
// The 3-argument lowering did NOT check that first argument, so it lowered
// whatever it was handed into a string operand: `int.TryParse(16, 16, out
// v)` emitted
//
//   %str.data = load ptr, i32 16, align 8
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// — the compiler ABORTED on a program that compiled before the 3-argument
// lowering existed (the base arm dropped the call, because no arm matched a
// 3-argument `TryParse` at all). The two-argument `TryParse(s, out result)`
// arm had the same hole and aborted the same way.
//
// These rows assert the COMPILER'S OUTCOME (it must compile, and it must
// never reach LLVM's IR verifier) — not the program's output: the dropped
// call prints "False"/"0" on the aborting arm's predecessor as well, so an
// output-only assertion would be green on both arms and pin nothing.
// ---------------------------------------------------------------------------

// The `int` first argument that aborted the compiler.
BEZEL_TEST(parse_radix_binding_7285,
           tryparse_radix_form_with_an_int_first_argument_does_not_abort) {
    JitResult r = runAot(tryParseFirstArgProg("16", "16"));
    expectCompiledWithoutIRFailure(r);
    // The call is DROPPED (it is not a `TryParse` call at all), so the out
    // slot keeps its zero-initialized value — the base arm's behaviour.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n0\n"));
}

// The `bool` first argument — the same shape, a different type.
BEZEL_TEST(parse_radix_binding_7285,
           tryparse_radix_form_with_a_bool_first_argument_does_not_abort) {
    JitResult r = runAot(tryParseFirstArgProg("true", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n0\n"));
}

// The two-argument `TryParse(s, out result)` form has the same hole, and it
// aborted the compiler on the BASE arm too.
BEZEL_TEST(parse_radix_binding_7285,
           two_argument_tryparse_with_a_non_string_first_argument_does_not_abort) {
    JitResult r = runAot(tryParseTwoArgFirstArgProg("16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n0\n"));
}

// The POSITIVE CONTROL: the correct shape still parses. This is the row that
// pins the fix's value — the guard must not swallow a legal call.
BEZEL_TEST(parse_radix_binding_7285,
           tryparse_radix_form_with_a_string_first_argument_still_parses_hex) {
    JitResult r = runAot(tryParseFirstArgProg("\"ff\"", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n255\n"));
}

// The `Parse(s, radix)` sibling: a non-string first argument is dropped, not
// aborted (its arm has always guarded the first argument).
BEZEL_TEST(parse_radix_binding_7285,
           parse_radix_form_with_a_non_string_first_argument_does_not_abort) {
    JitResult r = runAot(parseFirstArgProg("16", "16"));
    expectCompiledWithoutIRFailure(r);
    // The whole `Console.WriteLine` statement emits nothing — not even the
    // line terminator (the #3079 warning) — so the program prints nothing.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// ---------------------------------------------------------------------------
// THE DIAGNOSTIC quotes the RECEIVER'S OWN declared form.
//
// stdlib §9.1 (v0.344) declares the Int32 shape and says of the others:
// "The example below shows `Int32`; the others have analogous shapes with
// their respective `MinValue`/`MaxValue`" — i.e. `byte`'s shape is
// `Parse(s: string, radix: int): byte` and
// `TryParse(s: string, radix: int, out result: byte): bool`.
//
// The diagnostic used to quote the Int32 shape for EVERY receiver, so
// `byte.Parse`'s refusal cited a declaration stdlib §9.1 does not make.
// ---------------------------------------------------------------------------

BEZEL_TEST(parse_radix_binding_7285,
           byte_radix_diagnostic_quotes_the_receivers_own_declared_form) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(byte.Parse(\"ff\", true));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Parse(s: string, radix: int): byte`"));
    // The Int32 shape is NOT what stdlib §9.1 declares for `byte`.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "`Parse(s: string, radix: int): int`"));
}

BEZEL_TEST(parse_radix_binding_7285,
           byte_tryparse_radix_diagnostic_quotes_the_receivers_own_declared_form) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: byte := 0;\n"
        "    var ok: bool := byte.TryParse(\"ff\", true, out v);\n"
        "    Console.WriteLine(ok);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`TryParse(s: string, radix: int, out result: byte): bool`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "`TryParse(s: string, radix: int, out result: int): bool`"));
}

// ---------------------------------------------------------------------------
// AN ENUM-CONSTANT RADIX — the flip the fix-forward declares.
//
// stdlib §9.1 declares `radix: int`, so the argument must convert to an
// `int` implicitly. An enum constant does not: spec §10.2 (v0.344) says
// "Within the enum's own value initializers, a reference to one of the
// enum's constants, bare (`A`) or qualified (`E.A`), denotes that constant's
// underlying value at the underlying type, not a value of the enum. […]
// Everywhere else, member bodies included, an enum constant is a value of the
// enum type", and spec §5.1 (v0.344) lists "Between an enum and its
// underlying integer" among the conversions that are NEVER implicit. Spec
// §5.4 (v0.344) says the same from the other side: "Enums require explicit
// casts to and from their underlying integer type".
//
// So the radix argument is an enum-typed value at the call site, it converts
// to `int` only by an EXPLICIT cast (`int(E.A)`), and §22.6 makes a call with
// no reachable candidate a compile error.
//
// This is a BEHAVIOUR FLIP: on the base arm `int.Parse("ff", E.A)` COMPILED
// and printed 255 (the base arm truncated the enum's underlying value to
// i32 without checking it). The flip is the point of the radix rule, and
// the row below pins it.
//
// The mirror sites agree: `Convert.ToString(255, E.A)` (stdlib §2.3) and
// `(255).ToString(E.A)` (stdlib §9.1) were ALREADY refused before this fix,
// so all three radix sites now return the same verdict.
// ---------------------------------------------------------------------------

BEZEL_TEST(parse_radix_binding_7285, enum_constant_radix_is_rejected) {
    std::string prog =
        "module M;\n"
        "enum E\n"
        "  A := 16;\n"
        "end E;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(int.Parse(\"ff\", E.A));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "never implicitly convertible to an `int`"));
}

// The MIRROR site (stdlib §2.3) refuses the same argument — the two sites
// agree (a CONTROL: green on both arms, because the mirror's check lives in
// sema and did not change).
BEZEL_TEST(parse_radix_binding_7285,
           mirror_site_refuses_an_enum_constant_radix) {
    std::string prog =
        "module M;\n"
        "enum E\n"
        "  A := 16;\n"
        "end E;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Convert.ToString(255, E.A));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    expectRejected(r, "no matching overload of `Convert.ToString`");
}

// ---------------------------------------------------------------------------
// An IMPORTED class-level `const` as the radix.
//
// The radix's value reaches the lowering through the symbol sema recorded
// for the node: `sema::ConstMemberSymbol`. For an IMPORTED const the
// declaration does not exist in the importing compilation at all — the
// ModuleLoader has to have carried the value (`ConstMemberSymbol::intValue`)
// across the module boundary. If it had not, a legal program would be
// REFUSED, so the row below pins that the imported branch of the §17.14
// fold works.
// ---------------------------------------------------------------------------

namespace {

namespace fs7285 = std::filesystem;

// The library module: a class-level `const` (stdlib §22.12) of a `uint`
// type — a type that reaches an `int` parameter only by §5.1's constant
// narrowing, so the fold must carry its VALUE across the module boundary.
const char* kImportedConstLib7285 =
    "module Lib;\n"
    "public class Cfg7285\n"
    "  public static const K: uint := 16u;\n"
    "end Cfg7285;\n";

// The importing module: the radix is `Cfg7285.K`, an IMPORTED constant.
const char* kImportedConstApp7285 =
    "module App;\n"
    "import Lib;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    Console.WriteLine(int.Parse(\"ff\", Cfg7285.K));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

const fs7285::path& wtDir7285() { return wtScratchDir("wt-7285-radix"); }

void writeSource7285(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile the library to an object and the importer to an executable; return
// the executable's path, or the empty string when a compile failed.
std::string buildImported7285(const std::string& libSrc,
                               const std::string& appSrc) {
    const fs7285::path dir = wtDir7285();
    const std::string libStem = unique_stem("bezel_7285lib_");
    const std::string libSrcPath = (dir / (libStem + "_lib.bzl")).string();
    const std::string libObjPath = (dir / (libStem + "_lib.o")).string();
    const std::string appStem = unique_stem("bezel_7285app_");
    const std::string appSrcPath = (dir / (appStem + "_app.bzl")).string();
    const std::string appExePath = (dir / appStem).string();
    writeSource7285(libSrcPath, libSrc);
    writeSource7285(appSrcPath, appSrc);
    bezel::Driver libDriver;
    if (!libDriver.compileObject(libSrcPath, libObjPath).success)
        return std::string();
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep;
    dep.metadataPath = libObjPath;
    dep.objectPath = libObjPath;
    deps.push_back(dep);
    bezel::Driver appDriver;
    if (!bezel::test::compileFileForTest(appDriver, appSrcPath, appExePath,
                                         deps)
             .success)
        return std::string();
    return appExePath;
}

// The executable's stdout.
std::string captureStdout7285(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>/dev/null").c_str(), "r");
    if (!p) return out;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    pclose(p);
    return out;
}

}  // namespace

BEZEL_TEST(parse_radix_binding_7285,
           imported_class_const_radix_still_parses_hex) {
    const std::string exe = buildImported7285(kImportedConstLib7285,
                                               kImportedConstApp7285);
    BEZEL_EXPECT_FALSE(exe.empty());
    if (exe.empty()) return;
    BEZEL_EXPECT_EQ(captureStdout7285(exe), std::string("255\n"));
}
