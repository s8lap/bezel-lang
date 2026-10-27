// Regression tests for #7271 (spec §5.1 / §22.6 / §17.14, v0.344; stdlib
// §9.1 / §2.3, v0.344): the INTEGER receiver's `ToString(radix: int)` never
// checked that its radix argument BINDS to the declared `int` parameter.
//
// THE DEFECT. stdlib §9.1 declares three one-argument forms on an integer
// receiver — `ToString()`, `ToString(format: string)` and
// `ToString(radix: int)` — and codegen told them apart by inspecting the
// argument: an `int`/`long` argument took the radix path, a string LITERAL
// took the format path, and EVERYTHING ELSE fell through to the no-format
// renderer. A radix argument that binds to no declared parameter was
// therefore SILENTLY ABSORBED and the program printed the DECIMAL text of the
// receiver:
//
//   Console.WriteLine((255).ToString(16u));   // printed "255" (decimal)
//
// §22.6: "an argument that does not bind to a parameter type is a compile
// error" — never a silently degraded call.
//
// THE VERDICT §5.1 GIVES. The `radix` parameter is an `int`, and §5.1's
// conversion table admits an argument by IDENTITY (`int`), by the widening
// table (`sbyte`, `byte`, `short`, `ushort`, `char` → `int`), or by §5.1's
// CONSTANT NARROWING — "the source may be a compile-time constant of ANY
// integer type — not only `int` — and the target any integer type whose
// range contains the value, ACROSS SIGNEDNESS (… a `uint`-typed constant `7`
// is admissible where `int` is expected)". So:
//
//   * a `uint` LITERAL is ADMITTED — it is a §17.14 constant whose value
//     fits `int` — and the call renders the RADIX form ("ff"), not the
//     decimal;
//   * a runtime `uint` (a value, not a constant) binds to nothing → a
//     COMPILE ERROR, exactly as at the mirror site.
//
// THE MIRROR SITE. stdlib §2.3's `Convert.ToString(value: int, toBase: int)`
// says "`ToString(value, toBase)` returns the text `value.ToString(toBase)`
// returns" — so the two spellings must AGREE. The mirror site already did
// (§2.3's check lives in sema and already rejected a runtime `uint`/`ulong`/
// `long`/`double`/`bool` radix); the primitive receiver's site did not, so
// `(255).ToString(16u)` printed "255" while `Convert.ToString(255, 16u)`
// printed "ff". The rows below pin BOTH sides of that agreement.
//
// The `int` radix rows are CONTROLS: they are green on both arms and prove
// the fix did not break the good path.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The house diagnostic: the argument, the type stdlib §9.1 declares there,
// and the §5.1 rule that decides the verdict.
const char* kRadixNeedle = "so the `radix` argument must be an `int`";

// A `radix` argument of type `argTy` (a literal when `lit`, otherwise a
// runtime local of that type).
std::string progWithRadix(const std::string& decls, const std::string& decl,
                           const std::string& radix) {
    return "module M;\n" + decls +
           "class C\n"
           "  public static method Main(): int\n" + decl +
           "    Console.WriteLine((255).ToString(" + radix + "));\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectRejected(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// ---------------------------------------------------------------------------
// The ADMITTED forms: a §17.14 constant of a type that reaches `int` only by
// §5.1's constant narrowing renders the RADIX form.
// ---------------------------------------------------------------------------

// The issue's own repro: the `uint` literal is a compile-time constant whose
// value fits `int`, so §5.1 admits it — and stdlib §9.1's radix rendering
// applies. Pre-fix it printed the DECIMAL "255".
BEZEL_TEST(uint_radix_rejected_7271, uint_literal_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithRadix("", "", "16u")).stdoutText,
                    std::string("ff\n"));
}

// The same for the other constant-narrowed spelling.
BEZEL_TEST(uint_radix_rejected_7271, ulong_literal_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithRadix("", "", "16UL")).stdoutText,
                    std::string("ff\n"));
}

// A class-level `const` of the narrowable type (stdlib §22.12) is a §17.14
// constant too.
BEZEL_TEST(uint_radix_rejected_7271, uint_const_member_radix_renders_the_radix_form) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static const K: uint := 16;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine((255).ToString(C.K));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAot(prog).stdoutText, std::string("ff\n"));
}

// A `byte` constant reaches `int` by §5.1's WIDENING table.
BEZEL_TEST(uint_radix_rejected_7271, byte_const_member_radix_renders_the_radix_form) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static const K: byte := 16;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine((255).ToString(C.K));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAot(prog).stdoutText, std::string("ff\n"));
}

// The NAMED spelling of the admitted form behaves like the positional one.
BEZEL_TEST(uint_radix_rejected_7271, uint_literal_named_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithRadix("", "", "radix := 16u")).stdoutText,
                    std::string("ff\n"));
}

// ---------------------------------------------------------------------------
// The REJECTED forms: an argument that binds to no declared parameter.
// ---------------------------------------------------------------------------

// A runtime `uint` is not a compile-time constant, so §5.1's constant
// narrowing does not reach it and §5.1's never-implicit list governs
// ("between signed and unsigned types of the same or narrower width … is
// never implicit").
BEZEL_TEST(uint_radix_rejected_7271, runtime_uint_radix_is_rejected) {
    JitResult r = runAot(progWithRadix(
        "", "    var radix: uint := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `uint` reaches an `int` parameter only as a compile-time constant"));
}

// The same for `ulong` and for `long` (both reach an `int` parameter only by
// §5.1's constant narrowing).
BEZEL_TEST(uint_radix_rejected_7271, runtime_ulong_radix_is_rejected) {
    JitResult r = runAot(progWithRadix(
        "", "    var radix: ulong := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `ulong` reaches an `int` parameter only as a compile-time constant"));
}

BEZEL_TEST(uint_radix_rejected_7271, runtime_long_radix_is_rejected) {
    JitResult r = runAot(progWithRadix(
        "", "    var radix: long := 16;\n", "radix"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `long` reaches an `int` parameter only as a compile-time constant"));
}

// A `double` and a `bool` bind to NEITHER §9.1 overload.
BEZEL_TEST(uint_radix_rejected_7271, double_radix_is_rejected) {
    JitResult r = runAot(progWithRadix("", "", "16.0"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `double` is never implicitly convertible to an `int`"));
}

BEZEL_TEST(uint_radix_rejected_7271, bool_radix_is_rejected) {
    JitResult r = runAot(progWithRadix("", "", "true"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `bool` is never implicitly convertible to an `int`"));
}

// A constant whose value does not fit `int`'s range is a COMPILE ERROR at
// the conversion site (§5.1) — never a silent truncation.
BEZEL_TEST(uint_radix_rejected_7271, out_of_range_constant_radix_is_rejected) {
    JitResult r = runAot(progWithRadix("", "", "4000000000u"));
    expectRejected(r, kRadixNeedle);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the compile-time constant's value does not fit `int`'s range"));
}

// The NAMED spelling of a rejected form is rejected too.
BEZEL_TEST(uint_radix_rejected_7271, runtime_uint_named_radix_is_rejected) {
    JitResult r = runAot(progWithRadix(
        "", "    var radix: uint := 16;\n", "radix := radix"));
    expectRejected(r, kRadixNeedle);
}

// ---------------------------------------------------------------------------
// The CONTROLS — green on BOTH arms.
// ---------------------------------------------------------------------------

// The `int` radix still renders hex.
BEZEL_TEST(uint_radix_rejected_7271, int_radix_still_renders_hex) {
    BEZEL_EXPECT_EQ(runAot(progWithRadix("", "", "16")).stdoutText,
                    std::string("ff\n"));
}

// The `long` constant radix still renders hex (it was admitted before).
BEZEL_TEST(uint_radix_rejected_7271, long_literal_radix_still_renders_hex) {
    BEZEL_EXPECT_EQ(runAot(progWithRadix("", "", "16L")).stdoutText,
                    std::string("ff\n"));
}

// The mirror site agrees with the primitive site on the admitted `uint`
// literal (stdlib §2.3: "`ToString(value, toBase)` returns the text
// `value.ToString(toBase)` returns").
BEZEL_TEST(uint_radix_rejected_7271, mirror_site_agrees_on_the_uint_literal) {
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

// The mirror site's `ulong` VALUE rejection (issue #7266's rule — the `value`
// parameter, not the radix) is untouched by this fix.
BEZEL_TEST(uint_radix_rejected_7271, mirror_site_still_rejects_a_ulong_value) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var g: ulong := 10;\n"
        "    Console.WriteLine(Convert.ToString(g, 10));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
}

// The mirror site still rejects a runtime `uint` radix — the same rule the
// primitive site now applies.
BEZEL_TEST(uint_radix_rejected_7271, mirror_site_still_rejects_a_runtime_uint_radix) {
    std::string prog =
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: uint := 16;\n"
        "    Console.WriteLine(Convert.ToString(255, radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
}

// The OTHER §9.1 overload — `ToString(format: string)` — is untouched: a
// format argument is not a radix argument and must not be refused.
BEZEL_TEST(uint_radix_rejected_7271, format_overload_is_untouched) {
    BEZEL_EXPECT_EQ(runAot(progWithRadix("", "", "\"X\"")).stdoutText,
                    std::string("FF\n"));
}
