// Regression tests for #7286 (spec §5.1 / §7.1 / §22.6 / §17.14, v0.344;
// stdlib §9.1 / §2.3 / §2.18, v0.344): the INTEGER receiver's one-argument
// `ToString(...)` still absorbed FOUR argument shapes it does not admit.
//
// THE DEFECT. stdlib §9.1 declares three one-argument forms on an integer
// receiver — `ToString()`, `ToString(format: string)` and
// `ToString(radix: int)` — and codegen told them apart by inspecting the
// argument. Four shapes were still SILENTLY ABSORBED:
//
//   B1  var r: int? := 16;  (255).ToString(r)   compiled, printed "255"
//   B2  (255).ToString(8u + 8u)                  compiled, printed "255"
//   B3  var f: string := "X"; (255).ToString(f)  compiled, printed "255"
//   C4  var r: int := 16;  (255).ToString(r)    compiled, printed "255"
//
// WHAT THE SPEC SAYS ABOUT EACH.
//
//   B1 — §5.1's nullable conversions "lift only to the nullable form": `S?`
//        converts to `T?` whenever `S` converts to `T`, so an UN-NARROWED
//        `int?` reaches NEITHER `ToString(radix: int)` nor
//        `ToString(format: string)`; §7.1's "the compiler enforces the
//        distinction at every assignment, parameter pass, and return" is the
//        rule that refuses it; and §22.6: "If no candidate is applicable at
//        all — no candidate is reachable: a conversion §5.1 does not admit as
//        implicit … the call is a compile error with no candidates cited." So
//        an un-narrowed B1 is a COMPILE ERROR — reported by the §7
//        NULLABILITY GATE (#6782), the house's own gate for a nullable
//        argument at a non-nullable parameter.
//
//        A §7.4-NARROWED read is a different thing, and §22.6 makes it bind:
//        "The argument's static type is its type at the call: a read of a
//        binding §7.4 has narrowed non-null is of its non-nullable type … so a
//        narrowed `d: Dog?` is exact at a `Dog` parameter." §7.4 establishes
//        the narrowing by a null test ("`x <> null` or `x is not null`") AND
//        by the declaration's assignment ("and from a declaration whose
//        initializer is such an `expr` — `var x: T? := expr;` … which is the
//        declaration's assignment"). So `var radix: int? := 16;` narrows
//        `radix`, and BOTH spellings of the narrowed read are ADMITTED and
//        render the radix / format form.
//
//   B2 — §5.1's constant narrowing is VALUE-based: "the source may be a
//        compile-time constant of any integer type — not only `int` — and the
//        target any integer type whose range contains the value, across
//        signedness". `8u + 8u` is a §17.14 constant expression of value 16,
//        so it BINDS at the `int` parameter; and stdlib §9.1: "`ToString(radix)`
//        emits the lower-case digits". So B2 renders the RADIX form ("ff").
//        The binding already computed the folded value — the RENDERER ignored
//        it, reading the argument as an LLVM `ConstantInt`, which cannot see
//        the value of a constant EXPRESSION.
//
//   B3 — stdlib §9.1 declares `public pure method ToString(format: string):
//        string;` and a `string` argument binds to it EXACTLY (§22.6: the
//        radix overload's `int` parameter admits no `string`). The overload's
//        parameter is a `string` — the VALUE, not the literal text — so the
//        rendering is a RUNTIME one: the format is read at run time and the
//        rendering dispatched on it. So B3 renders "FF", what the LITERAL
//        format spelling renders.
//
//   C4 — `int` is the identity at the `int` parameter, so the argument BINDS;
//        and stdlib §9.1: "`ToString(radix)` emits the lower-case digits … and
//        takes the same radix set" (2, 8, 10, 16). So C4 renders the RADIX
//        form ("ff"), chosen at RUN time.
//
// The rows that use none of these four shapes are CONTROLS: green on both
// arms. The `radix`/`tostring`/`convert` filters are re-run green.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A `ToString` call on an `int` receiver, over the given argument.
std::string progWithArg(const std::string& decls, const std::string& decl,
                         const std::string& arg) {
    return "module M;\n" + decls +
           "class C\n"
           "  public static method Main(): int\n" + decl +
           "    Console.WriteLine((255).ToString(" + arg + "));\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same call over a receiver of another primitive type — the §9.1 integer
// receivers (`uint`, `long`, `ulong`, `byte`, `sbyte`, `short`, `ushort`)
// that carry the same `ToString(format: string)` / `ToString(radix: int)`
// pair as `Int32`.
std::string progWithRecv(const std::string& recv, const std::string& val,
                          const std::string& decls, const std::string& decl,
                          const std::string& arg) {
    return "module M;\n" + decls +
           "class C\n"
           "  public static method Main(): int\n" + decl +
           "    var v: " + recv + " := " + val + ";\n" +
           "    Console.WriteLine(v.ToString(" + arg + "));\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A program that must RUN CLEANLY — no crash, no sanitizer report — and
// print the expected text.
//
// The exit-status assertion is LOAD-BEARING for the memory-safety rows: under
// `BEZEL_AOT_FORCE_SANITIZE=address` an AddressSanitizer report replaces the
// program's own exit status with the sanitizer's, so a row that asserted only
// the OUTPUT would still fail on a crashing arm — but a row that asserted
// only "it compiled" would pass. Asserting both is what pins the defect.
void expectCleanRunWithOutput(const std::string& src,
                               const std::string& want) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The bounded padding width: the rendering buffer's capacity (65), so the
// padded text is 62 zeros and the three digits of "255".
const std::string& boundedWidthText() {
    static const std::string kText = std::string(62, '0') + "255";
    return kText;
}

// A REFUSAL: the compile fails with a LOCATED diagnostic — not a compiler
// crash, not a hang, not a sanitizer report — the program does NOT run, and
// nothing reaches stdout.
//
// The exit-status assertion is load-bearing: `failedAtCompileTimeWithRealLocation`
// alone already excludes the crash / hang / ASan outcomes, but the "did not
// run" pair is what pins that the program was never EXECUTED (a rejected
// compile cannot have run).
void expectRefused(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.empty());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// ---------------------------------------------------------------------------
// B1 — an UN-NARROWED NULLABLE radix binds to no §9.1 parameter (§5.1 / §7.1
// / §22.6); a §7.4-NARROWED one binds EXACTLY (§7.4 / §22.6).
// ---------------------------------------------------------------------------

// The declaration's assignment NARROWS the local (§7.4: "and from a
// declaration whose initializer is such an `expr` — `var x: T? := expr;` …
// which is the declaration's assignment"), so the read's type at the call is
// its non-nullable type (§22.6) and the call BINDS: the radix form.
BEZEL_TEST(radix_residual_shapes_7286, nullable_int_radix_narrowed_by_its_declaration_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

// The NAMED spelling of the same narrowed read is admitted too.
BEZEL_TEST(radix_residual_shapes_7286, nullable_int_named_radix_narrowed_by_its_declaration_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: int? := 16;\n", "radix := radix")).stdoutText,
                    std::string("ff\n"));
}

// A narrowed nullable STRING is the FORMAT overload's argument: `string?`
// narrowed is `string` at the call (§22.6), so the format form renders.
BEZEL_TEST(radix_residual_shapes_7286, nullable_string_format_narrowed_by_its_declaration_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var fmt: string? := \"X\";\n", "fmt")).stdoutText,
                    std::string("FF\n"));
}

// A NULL TEST narrows the true branch (§7.4: "a null test in either
// spelling — `x <> null` or `x is not null` (narrowing the true branch)"),
// so the read is of its non-nullable type at the call (§22.6) and the call
// BINDS: the radix form.
BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := 16;\n"
        "    if radix <> null then\n"
        "      Console.WriteLine((255).ToString(radix));\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("ff\n"));
}

// The ELSE branch of `if radix = null` narrows it too (§7.4: "`x = null` …
// narrowing the *false* branch").
BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_in_the_else_branch_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := 16;\n"
        "    if radix = null then\n"
        "      Console.WriteLine(\"none\");\n"
        "    else\n"
        "      Console.WriteLine((255).ToString(radix));\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("ff\n"));
}

// A narrowed nullable STRING format renders the FORMAT form.
BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_string_format_renders_the_format_form) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string? := \"X\";\n"
        "    if fmt <> null then\n"
        "      Console.WriteLine((255).ToString(fmt));\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("FF\n"));
}

// The house's own nullability gate REJECTS the un-narrowed nullable
// argument — "the argument to `int.ToString` is nullable but the target type
// is non-nullable (§7)" — and the primitive site must not contradict it.
BEZEL_TEST(radix_residual_shapes_7286, un_narrowed_nullable_radix_is_still_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := null;\n"
        "    Console.WriteLine((255).ToString(radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is nullable but the target type is non-nullable"));
}

// fix #7296 (§7 / §7.4 / §22.6, v0.344): the §7 nullability gate's registry
// now carries the §9.1 rows for the OTHER integer receivers (`uint`, `long`,
// `ulong`, `byte`, `sbyte`, `short`, `ushort` — stdlib §9.1 declares
// "SByte, Byte, Int16, UInt16, Int32, UInt32, Int64, UInt64", and every one
// of them carries the same `ToString(format: string)` / `ToString(radix: int)`
// pair), and sema's `primitiveKeywordHead` names them. So the GATE owns that
// argument position at those receivers too, and its diagnostic is the house's
// §7 one — the same verdict the `int` receiver gives, and the same verdict
// the mirror gives.
//
// (This row's EXPECTATION CHANGED: it used to pin the primitive site's own
// "a NULLABLE argument binds to neither" diagnostic, which owned the
// position while the registry had no row for `UInt32`.)
BEZEL_TEST(radix_residual_shapes_7286, un_narrowed_nullable_radix_at_a_uint_receiver_is_refused_by_the_section_7_gate) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: uint := 1;\n"
        "    var radix: int? := null;\n"
        "    Console.WriteLine(v.ToString(radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.empty());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `uint.ToString` is nullable but the target type "
           "is non-nullable"));
}

// §7.4 is FLOW-SENSITIVE: "the variable is reassigned from a nullable-typed
// expression" invalidates the narrowing, so the read is `int?` again at the
// call and the call is REFUSED — the same verdict the MIRROR gives for the
// same shape (the two spellings must not diverge in ADMISSION).
BEZEL_TEST(radix_residual_shapes_7286, nullable_radix_after_its_narrowing_is_invalidated_is_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := 16;\n"
        "    radix := null;\n"
        "    Console.WriteLine((255).ToString(radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is nullable but the target type is non-nullable"));
}

// The MIRROR SITE admits the same narrowed nullable, so the two spellings of
// one overload AGREE IN ADMISSION (§2.3's `Convert.ToString(value, toBase)`).
//
// The row pins the ADMISSION, not the mirror's TEXT: the mirror's rendering
// is the sibling lane's own business (its commit d6d38b64 renders the radix
// form for an admitted radix), and a row that pinned the old decimal text
// would break when that commit lands. What this row owns is the verdict:
// the call COMPILES and RUNS.
BEZEL_TEST(radix_residual_shapes_7286, the_mirror_site_admits_the_same_narrowed_nullable) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := 16;\n"
        "    if radix <> null then\n"
        "      Console.WriteLine(Convert.ToString(255, radix));\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(!r.stdoutText.empty());
}

// ---------------------------------------------------------------------------
// B2 — a FOLDED constant expression's value drives the rendering (§5.1 /
// §17.14 / stdlib §9.1). The binding already computed the folded value; the
// renderer ignored it.
// ---------------------------------------------------------------------------

BEZEL_TEST(radix_residual_shapes_7286, folded_constant_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg("", "", "8u + 8u")).stdoutText,
                    std::string("ff\n"));
}

// The same for a folded expression of another constant-narrowed type.
BEZEL_TEST(radix_residual_shapes_7286, folded_long_constant_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg("", "", "8L + 8L")).stdoutText,
                    std::string("ff\n"));
}

// ---------------------------------------------------------------------------
// C4 — a RUNTIME radix renders the radix form, chosen at RUN time
// (stdlib §9.1: the admitted radix set is 2, 8, 10, 16).
// ---------------------------------------------------------------------------

BEZEL_TEST(radix_residual_shapes_7286, runtime_int_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: int := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

// The radix 8 — the octal form.
BEZEL_TEST(radix_residual_shapes_7286, runtime_int_radix_renders_the_octal_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: int := 8;\n", "radix")).stdoutText,
                    std::string("377\n"));
}

// The radix 2 — the binary form.
BEZEL_TEST(radix_residual_shapes_7286, runtime_int_radix_renders_the_binary_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: int := 2;\n", "radix")).stdoutText,
                    std::string("11111111\n"));
}

// A runtime radix that is not a compile-time constant is still the IDENTITY at
// the `int` parameter — so a `byte`-typed runtime radix is admitted too.
BEZEL_TEST(radix_residual_shapes_7286, runtime_byte_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: byte := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

// The NAMED spelling of the runtime radix renders the radix form too.
BEZEL_TEST(radix_residual_shapes_7286, runtime_int_named_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: int := 16;\n", "radix := radix")).stdoutText,
                    std::string("ff\n"));
}

// A NEGATIVE value renders with its sign, as §9.1's radix text does ("a `-`
// for a negative value followed by the magnitude's digits").
BEZEL_TEST(radix_residual_shapes_7286, runtime_radix_of_a_negative_value_keeps_the_sign) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int := 16;\n"
        "    Console.WriteLine((-255).ToString(radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("-ff\n"));
}

// ---------------------------------------------------------------------------
// B3 — the FORMAT overload with a RUNTIME format string (stdlib §9.1 /
// §2.18). The parameter is a `string` — the VALUE, not the literal text — so
// the rendering is a runtime one.
// ---------------------------------------------------------------------------

BEZEL_TEST(radix_residual_shapes_7286, runtime_format_string_renders_the_format_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var fmt: string := \"X\";\n", "fmt")).stdoutText,
                    std::string("FF\n"));
}

// The decimal specifier — the same text the no-format rendering gives.
BEZEL_TEST(radix_residual_shapes_7286, runtime_format_string_decimal_specifier) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var fmt: string := \"D\";\n", "fmt")).stdoutText,
                    std::string("255\n"));
}

// The digit-suffixed specifier: `D4` is the minimum digit count, zero-padded
// (stdlib §2.18's integer row) — what the LITERAL spelling renders.
BEZEL_TEST(radix_residual_shapes_7286, runtime_format_string_with_a_width) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var fmt: string := \"D4\";\n", "fmt")).stdoutText,
                    std::string("0255\n"));
}

// The hex specifier with a width.
BEZEL_TEST(radix_residual_shapes_7286, runtime_format_string_hex_with_a_width) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var fmt: string := \"X4\";\n", "fmt")).stdoutText,
                    std::string("00FF\n"));
}

// The format is read at RUN time: the same call site with a different value
// renders differently — so the rendering is not constant-folded.
BEZEL_TEST(radix_residual_shapes_7286, runtime_format_string_is_read_at_run_time) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"X\";\n"
        "    Console.WriteLine((255).ToString(fmt));\n"
        "    fmt := \"D\";\n"
        "    Console.WriteLine((255).ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("FF\n255\n"));
}

// A specifier OUTSIDE stdlib §2.18's vocabulary raises `FormatException` —
// the same answer the LITERAL spelling gives for the same specifier, and the
// answer §2.18 states: "An unrecognized specifier raises `FormatException`
// (strict-fault, as everywhere)".
//
// THE TEXT MOVED (#7361): this row used to pin the FALLBACK — the no-format
// rendering ("255") — because the runtime spelling had no vocabulary of its
// own and rendered the plain text for every specifier it did not know. The
// literal spelling faulted for the same specifier, so the two spellings of
// one rule disagreed.
BEZEL_TEST(radix_residual_shapes_7286,
           runtime_format_string_unrecognized_specifier_raises_format_exception) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"Q\";\n"
        "    var result := 0;\n"
        "    try\n"
        "      var s: string := (255).ToString(fmt);\n"
        "      result := 99;\n"
        "    catch FormatException => result := 11;\n"
        "    end;\n"
        "    Console.WriteLine(result);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("11\n"));
}

// ---------------------------------------------------------------------------
// The MEMORY-SAFETY rows (the fix-forward's D1 and D2). These are NOT corpus
// rows: the corpus arms no sanitizer, and a corpus `reject` row is GREEN ON
// A CRASH. They are gtest rows run under
// `BEZEL_AOT_FORCE_SANITIZE=address`, and each asserts that the program runs
// CLEANLY as well as what it prints.
//
// D1: the runtime format path read ONE BYTE PAST the utf8 buffer of an EMPTY
// format string (`bz_string_to_utf8z` allocates `n*4+1` bytes — ONE byte for
// a zero-length string — and the path took a `+1` GEP of it).
// ---------------------------------------------------------------------------

// The EMPTY LITERAL format: the literal path's own `if (!bezelFmt.empty())`
// guard let an empty literal fall through into the runtime path, so this
// spelling crashed too.
BEZEL_TEST(radix_residual_shapes_7286, empty_literal_format_runs_cleanly) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine((255).ToString(\"\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        std::string("255\n"));
}

// The EMPTY RUNTIME format: the same over-read, through the runtime path.
BEZEL_TEST(radix_residual_shapes_7286, empty_runtime_format_runs_cleanly) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"\";\n"
        "    Console.WriteLine((255).ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        std::string("255\n"));
}

// ---------------------------------------------------------------------------
// D2: the runtime format path's WIDTH was unbounded (`strtol` over the digit
// run, so "D99999999999" is 1215752191), and the shared snprintf helper
// copied snprintf's RETURN VALUE — the length the output WOULD have had —
// out of the 66-byte stack buffer, reading past it and copying ADJACENT
// STACK BYTES into the string the program receives.
// ---------------------------------------------------------------------------

// A width of 100 is beyond the rendering buffer, so it is bounded by it.
BEZEL_TEST(radix_residual_shapes_7286, runtime_format_width_beyond_the_buffer_is_bounded) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"D100\";\n"
        "    Console.WriteLine((255).ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        boundedWidthText() + "\n");
}

// A width of 67 is beyond it too.
BEZEL_TEST(radix_residual_shapes_7286, runtime_format_width_of_67_is_bounded) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"D67\";\n"
        "    Console.WriteLine((255).ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        boundedWidthText() + "\n");
}

// A width that does not fit in an `int` at all.
BEZEL_TEST(radix_residual_shapes_7286, runtime_format_huge_width_is_bounded) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"D99999999999\";\n"
        "    Console.WriteLine((255).ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        boundedWidthText() + "\n");
}

// The LITERAL spelling had the PRE-EXISTING version of the same overflow
// (the same helper shape, in the literal path's own inline copy): it is
// fixed too, and the two spellings of one overload now AGREE on the bound.
BEZEL_TEST(radix_residual_shapes_7286, literal_format_width_beyond_the_buffer_is_bounded) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine((255).ToString(\"D100\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        boundedWidthText() + "\n");
}

// The two spellings of one overload, side by side.
BEZEL_TEST(radix_residual_shapes_7286, the_two_format_spellings_agree_on_the_bounded_width) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"D100\";\n"
        "    Console.WriteLine((255).ToString(fmt));\n"
        "    Console.WriteLine((255).ToString(\"D100\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        boundedWidthText() + "\n" + boundedWidthText() + "\n");
}

// ---------------------------------------------------------------------------
// D5: the runtime format path fed an UNSIGNED receiver to a SIGNED printf
// conversion, so a `uint` of 4000000000 rendered as "-294967296" and a
// `ulong` of 18446744073709551615 as "-1" — where the no-format rendering of
// the same value (stdlib §9.1: "The no-format `ToString()` … emits the text
// `ToString(10)` emits") gives "4000000000" and "18446744073709551615".
// ---------------------------------------------------------------------------

BEZEL_TEST(radix_residual_shapes_7286, runtime_format_of_a_uint_agrees_with_the_no_format_rendering) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: uint := 4000000000;\n"
        "    Console.WriteLine(v.ToString());\n"
        "    var fmt: string := \"D\";\n"
        "    Console.WriteLine(v.ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        std::string("4000000000\n4000000000\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, runtime_format_of_a_ulong_renders_the_decimal_text) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: ulong := 18446744073709551615;\n"
        "    var fmt: string := \"D\";\n"
        "    Console.WriteLine(v.ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        std::string("18446744073709551615\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, runtime_format_of_a_ulong_renders_the_hex_text) {
    expectCleanRunWithOutput(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: ulong := 18446744073709551615;\n"
        "    var fmt: string := \"X\";\n"
        "    Console.WriteLine(v.ToString(fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        std::string("FFFFFFFFFFFFFFFF\n"));
}

// ---------------------------------------------------------------------------
// The CONTROLS — green on BOTH arms.
// ---------------------------------------------------------------------------

// The `int` literal radix still renders hex.
BEZEL_TEST(radix_residual_shapes_7286, int_literal_radix_still_renders_hex) {
    BEZEL_EXPECT_EQ(runAot(progWithArg("", "", "16")).stdoutText,
                    std::string("ff\n"));
}

// The `uint` literal radix (#7271's fix) still renders hex.
BEZEL_TEST(radix_residual_shapes_7286, uint_literal_radix_still_renders_hex) {
    BEZEL_EXPECT_EQ(runAot(progWithArg("", "", "16u")).stdoutText,
                    std::string("ff\n"));
}

// The LITERAL format spelling still renders the format form.
BEZEL_TEST(radix_residual_shapes_7286, format_literal_still_renders_the_format_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg("", "", "\"X\"")).stdoutText,
                    std::string("FF\n"));
}

// The runtime radix 10 — the admitted decimal radix — renders the decimal
// text, as it did before.
BEZEL_TEST(radix_residual_shapes_7286, runtime_radix_ten_still_renders_decimal) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    var radix: int := 10;\n", "radix")).stdoutText,
                    std::string("255\n"));
}

// An UNADMITTED radix RAISES `ArgumentException` (stdlib §9.1: "`ToString(radix)`
// … takes the same radix set, raising `ArgumentException` for any other").
//
// BEHAVIOUR FLIP (fix #7268, v0.345): this row used to pin the OLD behaviour
// — the radix 3 kept the DECIMAL fallback and the program printed "255".
// The §9.1 rule is now implemented, so the program raises instead, and the
// row says so. The catch arm is TYPED, so the row also pins the exception
// TYPE: an exception of any other type would not match it and would
// propagate (the program would then exit 70, not 1).
BEZEL_TEST(radix_residual_shapes_7286, runtime_unadmitted_radix_raises_argument_exception) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int := 3;\n"
        "    try\n"
        "      Console.WriteLine((255).ToString(radix));\n"
        "      return 0;\n"
        "    catch\n"
        "      ArgumentException => return 1;\n"
        "    end;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A `const` local's value is a §17.14 constant, so its radix renders the
// radix form (the compile-time path, unchanged).
BEZEL_TEST(radix_residual_shapes_7286, const_local_radix_renders_the_radix_form) {
    BEZEL_EXPECT_EQ(runAot(progWithArg(
        "", "    const radix: int := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

// The OVER-REJECTION CONTROL: a program that uses none of the four shapes is
// unchanged.
BEZEL_TEST(radix_residual_shapes_7286, a_program_without_the_four_shapes_is_unchanged) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine((255).ToString());\n"
        "    Console.WriteLine((255).ToString(16));\n"
        "    Console.WriteLine((255).ToString(\"X\"));\n"
        "    Console.WriteLine((255).ToString(8));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("255\nff\nFF\n377\n"));
}

// The MIRROR SITE (stdlib §2.3's `Convert.ToString(value, toBase)`) is
// untouched by this fix — the `uint` literal still renders hex there.
BEZEL_TEST(radix_residual_shapes_7286, mirror_site_still_renders_the_uint_literal_radix) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Convert.ToString(255, 16u));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("ff\n"));
}

// The mirror site's runtime-`uint` radix rejection (#7266's rule) is
// untouched by this fix.
BEZEL_TEST(radix_residual_shapes_7286, mirror_site_still_rejects_a_runtime_uint_radix) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: uint := 16;\n"
        "    Console.WriteLine(Convert.ToString(255, radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
}

// The mirror site refuses a `string` radix — a `string` binds to no §2.3
// `toBase: int` parameter.
BEZEL_TEST(radix_residual_shapes_7286, mirror_site_still_rejects_a_string_radix) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string := \"X\";\n"
        "    Console.WriteLine(Convert.ToString(255, fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
}

// ---------------------------------------------------------------------------
// fix #7296 — the NAMED spelling of stdlib §9.1's `ToString(radix: int)` /
// `ToString(format: string)`, and the §7 gate's rows for the other integer
// receivers (spec §7 / §7.1 / §7.4 / §22.6, stdlib §2.3 / §9.1, v0.344).
//
// THE DEFECT. The §7 nullability gate (`ExprTC::checkBuiltinNullableArgs`)
// SKIPPED every argument that carries a NAME — "a named argument binds by
// name, not position" — and `checkBuiltinNamedArgs` (#3398) leaves the
// primitive receivers UNCLAIMED ("Only `string` has a documented
// parameter-name surface here (stdlib §9.6); the other primitives are left
// unclaimed"). So at an `int` receiver a NAMED argument was neither
// name-validated nor nullability-checked: it bound POSITIONALLY (codegen
// lowers a built-in call by position — §22.5, #3423) and NOTHING owned the
// nullability decision. MEASURED on the pre-fix arm: `ToString(radix := r)`
// with an un-narrowed `int?` COMPILED and printed "255" — the decimal text —
// while the POSITIONAL spelling was refused and the MIRROR spelling
// (`Convert.ToString(value, toBase)`, stdlib §2.3) was refused too. Two
// spellings of ONE overload disagreed in ADMISSION.
//
// §22.6 (v0.344) is what the refusal follows from: "The argument's static
// type is its type at the call", and §5.1's nullable conversions "lift only
// to the nullable form", so an un-narrowed `int?` reaches NEITHER
// `ToString(radix: int)` nor `ToString(format: string)` — the call is a
// compile error. A §7.4-NARROWED read is of its NON-NULLABLE type at the
// call (§22.6), so it BINDS and renders the radix / format form.
//
// THE FIX. The gate may read an argument's POSITION exactly where the
// compiler does NOT honour the name there (the name is then neither matched
// nor rejected, and the argument binds positionally). One shared helper
// (`builtinCallHeadHonouringArgNames`) answers that question for the gate and
// for the named-argument binder alike.
// ---------------------------------------------------------------------------

// The NAMED spelling of an un-narrowed nullable radix is REFUSED — the §7
// gate's own diagnostic, the same one the positional spelling gives.
BEZEL_TEST(radix_residual_shapes_7286, named_unnarrowed_nullable_radix_is_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := null;\n"
        "    Console.WriteLine((255).ToString(radix := radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRefused(r, "the argument to `int.ToString` is nullable but the "
                     "target type is non-nullable");
}

// The NAMED spelling of an un-narrowed nullable FORMAT is refused too — the
// `ToString(format: string)` overload's parameter is a `string`, and §5.1
// lifts an un-narrowed `string?` only to the nullable form.
BEZEL_TEST(radix_residual_shapes_7286, named_unnarrowed_nullable_format_is_rejected) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var fmt: string? := null;\n"
        "    Console.WriteLine((255).ToString(format := fmt));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRefused(r, "the argument to `int.ToString` is nullable but the "
                     "target type is non-nullable");
}

// The two spellings of ONE overload (§9.1's `ToString(radix: int)` and
// §2.3's `Convert.ToString(value, toBase)`, which "returns the text
// `value.ToString(toBase)` returns") now AGREE IN ADMISSION on the NAMED
// spelling: the primitive site refuses the un-narrowed named nullable, and
// the mirror refuses it too. (The WORDING differs — the primitive site is
// the §7 gate's, the mirror's is the house's "no matching overload" — and
// that is what the reviewer ruled: the two sites must agree in VERDICT.)
BEZEL_TEST(radix_residual_shapes_7286, the_two_spellings_agree_on_the_named_un_narrowed_nullable) {
    JitResult prim = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := null;\n"
        "    Console.WriteLine((255).ToString(radix := radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRefused(prim, "is nullable but the target type is non-nullable");

    JitResult mirror = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := null;\n"
        "    Console.WriteLine(Convert.ToString(255, toBase := radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRefused(mirror, "no matching overload of `Convert.ToString`");
}

// The two spellings AGREE on the NARROWED named nullable as well: both admit
// it, and both render the radix form.
BEZEL_TEST(radix_residual_shapes_7286, the_two_spellings_agree_on_the_named_narrowed_nullable) {
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := 16;\n"
        "    Console.WriteLine((255).ToString(radix := radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("ff\n"));
    BEZEL_EXPECT_EQ(runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var radix: int? := 16;\n"
        "    Console.WriteLine(Convert.ToString(255, toBase := radix));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n").stdoutText,
                    std::string("ff\n"));
}

// The NAMED spelling of a NON-nullable radix is still admitted and still
// renders the radix form — the fix refuses the un-narrowed nullable, not
// the name.
BEZEL_TEST(radix_residual_shapes_7286, named_non_nullable_radix_is_still_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithArg("", "", "radix := 16")).stdoutText,
                    std::string("ff\n"));
}

// The NAMED spelling of a NON-nullable format is still admitted and still
// renders the format form.
BEZEL_TEST(radix_residual_shapes_7286, named_non_nullable_format_is_still_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithArg("", "", "format := \"X\"")).stdoutText,
                    std::string("FF\n"));
}

// ---------------------------------------------------------------------------
// The D3 RESIDUAL — the §7 gate's rows for the OTHER §9.1 integer receivers.
//
// stdlib §9.1 (v0.344) declares "SByte, Byte, Int16, UInt16, Int32, UInt32,
// Int64, UInt64", and every one of them carries §9.1's `ToString(format:
// string)` / `ToString(radix: int)` pair. The registry carried only
// `Int32`'s row and sema's `primitiveKeywordHead` named only `int`,
// `double`, `float`, `decimal`, `bool` and `char`, so at the other seven
// receivers NOTHING owned that argument position: an un-narrowed nullable
// was refused by the primitive site's own rejection (a SECOND spelling of a
// decision sema makes) and a §7.4-NARROWED one was OVER-REJECTED too —
// while the positional spelling at `int` and the mirror admitted it.
//
// With the rows and the `primitiveKeywordHead` entries, the §7 gate owns
// those positions: an un-narrowed nullable is refused by the GATE, and a
// narrowed one is ADMITTED and renders the radix form (§22.6).
// ---------------------------------------------------------------------------

BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_at_a_uint_receiver_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithRecv(
        "uint", "255", "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_at_a_long_receiver_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithRecv(
        "long", "255", "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_at_a_ulong_receiver_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithRecv(
        "ulong", "255", "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_at_a_byte_receiver_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithRecv(
        "byte", "255", "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_at_a_sbyte_receiver_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithRecv(
        "sbyte", "127", "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("7f\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_at_a_short_receiver_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithRecv(
        "short", "255", "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

BEZEL_TEST(radix_residual_shapes_7286, narrowed_nullable_radix_at_a_ushort_receiver_is_admitted) {
    BEZEL_EXPECT_EQ(runAot(progWithRecv(
        "ushort", "255", "", "    var radix: int? := 16;\n", "radix")).stdoutText,
                    std::string("ff\n"));
}

// The NAMED spelling at those seven receivers: an un-narrowed nullable is
// refused by the §7 gate — the name is not honoured there, so the argument
// binds positionally and the gate's decision applies to it.
BEZEL_TEST(radix_residual_shapes_7286, named_unnarrowed_nullable_radix_at_the_other_integer_receivers_is_rejected) {
    const char* heads[] = {"uint", "long", "ulong", "byte", "sbyte", "short",
                           "ushort"};
    for (const char* h : heads) {
        JitResult r = runAot(
            "module M;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    var v: " + std::string(h) + " := 1;\n"
            "    var radix: int? := null;\n"
            "    Console.WriteLine(v.ToString(radix := radix));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n");
        expectRefused(r, "is nullable but the target type is non-nullable");
    }
}

// The NAMED spelling at those seven receivers: a §7.4-NARROWED nullable is
// admitted and renders the radix form.
BEZEL_TEST(radix_residual_shapes_7286, named_narrowed_nullable_radix_at_the_other_integer_receivers_is_admitted) {
    struct Recv { const char* type; const char* value; const char* text; };
    const Recv recvs[] = {
        {"uint", "255", "ff"},  {"long", "255", "ff"}, {"ulong", "255", "ff"},
        {"byte", "255", "ff"},  {"sbyte", "127", "7f"}, {"short", "255", "ff"},
        {"ushort", "255", "ff"},
    };
    for (const Recv& r : recvs) {
        BEZEL_EXPECT_EQ(runAot(
            "module M;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    var v: " + std::string(r.type) + " := " + r.value + ";\n"
            "    var radix: int? := 16;\n"
            "    Console.WriteLine(v.ToString(radix := radix));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n").stdoutText,
                        std::string(r.text) + "\n");
    }
}

// The `string` receiver is NOT touched: it HAS a parameter-name surface
// (stdlib §9.6), and §9.6 declares `ToString(): string` — no one-argument
// form at all — so both the named and the positional one-argument spellings
// are refused by the arity check, exactly as before.
BEZEL_TEST(radix_residual_shapes_7286, the_string_receiver_keeps_its_no_argument_tostring_surface) {
    JitResult named = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var s: string := \"ab\";\n"
        "    Console.WriteLine(s.ToString(format := \"X\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRefused(named, "supplies 1 argument(s) but stdlib §9.6 declares "
                         "`ToString(): string`");

    JitResult positional = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var s: string := \"ab\";\n"
        "    Console.WriteLine(s.ToString(\"X\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRefused(positional, "supplies 1 argument(s) but stdlib §9.6 declares "
                              "`ToString(): string`");
}
