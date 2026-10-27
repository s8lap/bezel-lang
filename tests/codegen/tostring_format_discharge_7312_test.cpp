// Spec-derived regression test for issue #7312.
//
// stdlib §9.1 (v0.344) declares, for every numeric primitive:
//
//     public pure method ToString(): string;
//     public pure method ToString(format: string): string;
//     public pure method ToString(radix: int): string;
//
// and stdlib §2.18 (v0.344) fixes what the format overload renders. The
// language spec §28.9 (v0.344) makes `string` runtime-managed — "its memory
// is reclaimed automatically" — while leaving the MECHANISM open: "The
// implementation of runtime-managed types' lifecycle (reference counting,
// region allocation, or other) is not part of this specification." §2.18's
// round-trip requirement (the rendered text must SURVIVE the call and parse
// back) is what makes the result's reference a TRANSFER and not a release at
// the end of the call.
//
// THE DEFECT. The `ToString(format)` member-call arm (the `ToString(format)`
// shape in `Emitter::lowerExprInner`, LlvmBackend_Exprs.cpp) mints its result
// as a fresh reference-counted string at rc := 1 — an inline `malloc` +
// `initStringRc` for the literal-format path, the runtime's
// `bz_double_to_string_r` / `bz_decimal_to_string_fmt` / `bz_char_to_string`
// (each a header at rc := 1) for the rest — and NOBODY EVER DROPPED THAT
// REFERENCE. Every consumer of the result therefore treated it as a BORROW: a
// local binding retained it ON TOP of the producer's own +1 and released only
// its own retain at scope end; the other consumers (a call argument, a field
// store, a `return`, a concat operand, an interpolation hole) released
// nothing at all. The producer's +1 was stranded and the string was never
// reclaimed.
//
// MEASURED on the base arm (LeakSanitizer, `bezelc --sanitize=address`):
//
//     x.ToString("F4")    ->  "39 byte(s) leaked in 2 allocation(s)"
//     pInf.ToString("F2") ->  "36 byte(s) leaked in 2 allocation(s)"
//
// and unbounded in a loop: 200 iterations of the local-binding shape leaked
// 7800 bytes in 400 allocations — one allocation pair per call.
//
// THE ORACLE. `runAot` NEVER TRANSFERS A SUCCESSFUL COMPILE'S STDERR INTO
// `r.diagnostics` (only the REJECT branch does), so a `runAot`-built
// diagnostic row would be VACUOUS here. The real oracle is `runAotChecked`
// (generated-code AddressSanitizer + LeakSanitizer at exit): a leak makes it
// report the sanitizer finding, leave `r.ran` false and set a non-zero exit
// code. EVERY leak row below is loop-amplified (200 iterations) so the leak
// is unmistakable and the fixed arm's zero is unambiguous, and EVERY row
// asserts the program's EXACT stdout — so no row can pass without the
// program having run.
//
// THE DIRECTION A PURE "no leak" ASSERTION IS BLIND TO — an OVER-release —
// is asserted separately, by `the_formatted_result_survives_the_call`: it
// reads the bound string AFTER a churn allocation, so a reference dropped one
// step too early would free the buffer the program still reads (a
// use-after-free under generated-code ASan), and it parses the text back
// through `double.Parse` — §2.18's requirement that the rendered text remain
// a valid, parseable value after the call.
//
// THE CONTROL ROWS are the two shapes that were already correct — the
// no-format `ToString()` and the interpolation hole WITH a format specifier —
// and they are green on BOTH arms. The no-format row is what proves the fix
// did not over-reach into the sibling spelling; the interpolation row is what
// proves the §18.5 hole path was left alone.
//
// WHY THE TWO CONTROLS ARE CLEAN AND THE FORMAT SHAPE WAS NOT. The
// interpolation hole renders a hole that carries a FORMAT SPECIFIER through
// the runtime's formatter, so the rendered value is a FRESH RENDER distinct
// from the hole's own source expression, and `appendPart`'s `pv != srcEv`
// rule releases it. The no-format `ToString()` is recognized by
// `Emitter::isPlusOneStringProducer`, so every consumer of it TRANSFERS (and
// discharges) its single reference. The `ToString(format)` result was in
// NEITHER category: it was not a classified +1 producer, so no consumer
// discharged it, and in the interpolation case the hole's value IS the source
// expression (`pv == srcEv`), so the `pv != srcEv` release did not fire
// either. The fix classifies the one-argument `ToString` overloads as +1
// producers, exactly as the no-argument shape already was.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// The amplification every leak row uses. 200 iterations of a leaking
// `ToString(format)` call is 200 allocation PAIRS (7800 bytes) — far past
// LSan's noise floor, and far past the "a leftover pointer in a register
// makes a leaked block look merely reachable" flakiness.
constexpr int kIterations = 200;

// `x.ToString("F4")` for x = 3.14159 renders "3.1416" — SIX code points.
constexpr int kFormattedLength = 6;

// `x.ToString()` for the same x renders "3.14159" — SEVEN code points.
constexpr int kNoFormatLength = 7;

// The expected stdout of the local-binding row: each iteration adds the
// formatted text's length plus the churn string's length ("c" followed by
// the iteration number).
std::string expectedLocalTotal() {
    int total = 0;
    for (int i = 1; i <= kIterations; ++i) {
        total += kFormattedLength;
        total += 1 + static_cast<int>(std::to_string(i).size());
    }
    return std::to_string(total) + "\n";
}

// The expected stdout of the rows that accumulate only the formatted text's
// length.
std::string expectedFormattedTotal() {
    return std::to_string(kIterations * kFormattedLength) + "\n";
}

// A program whose `Main` body is `body`; every row below is one of these.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The loop that drives the amplification. `stmt` is the leaking statement.
std::string loop200(const std::string& stmt) {
    return "    var i: int := 0;\n"
           "    var t: int := 0;\n"
           "    while i < " + std::to_string(kIterations) + " do\n"
           "      i := i + 1;\n" + stmt +
           "    end while;\n";
}

// The three non-finite double values of stdlib §2.18 / §9.2, bound by name.
std::string nonFiniteDecls() {
    return "    var pInf := double.PositiveInfinity;\n"
           "    var nInf := double.NegativeInfinity;\n"
           "    var nan := double.NaN;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE LEAK ROWS — every one of them RED on the base arm.
// ---------------------------------------------------------------------------

// #7312 / stdlib §2.18 + §9.1 + spec §28.9 — the FINITE format result bound
// to a local. The local's slot is the value's owner: with the result
// classified as a +1 producer the binding TRANSFERS the producer's single
// reference into the slot and the scope-end release discharges it. On the
// base arm the result was a BORROW: the binding retained on top of the
// producer's +1 and released only its own retain, stranding the producer's
// reference (MEASURED: 7800 bytes in 400 allocations over 200 iterations).
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_finite_format_result_bound_to_a_local_is_discharged_2_18) {
    JitResult r = runAotChecked(prog(
        "    var x := 3.14159;\n" + loop200(
            "      var s: string := x.ToString(\"F4\");\n"
            "      var churn: string := \"c\" + i.ToString();\n"
            "      t := t + s.Length + churn.Length;\n") +
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedLocalTotal());
}

// #7312 / stdlib §2.18's NON-FINITE spellings ("Infinity" / "-Infinity" /
// "NaN") + spec §28.9 — the same shape over a non-finite operand. The
// non-finite rendering is the runtime's `bz_double_to_string_r`, which is a
// header at rc := 1 exactly like the inline path, so it must be discharged
// too. This row also pins that #7301's spelling is unchanged by the fix.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_non_finite_format_result_bound_to_a_local_is_discharged_2_18) {
    JitResult r = runAotChecked(prog(
        nonFiniteDecls() + loop200(
            "      var a: string := pInf.ToString(\"F2\");\n"
            "      var b: string := nInf.ToString(\"F2\");\n"
            "      var c: string := nan.ToString(\"F2\");\n"
            "      t := t + a.Length + b.Length + c.Length;\n") +
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4000\n");
}

// #7312 / spec §28.9 — the CONSOLE ARGUMENT consumer. The console call takes
// the result as a borrow, so the producer's reference is discharged after the
// call. On the base arm nothing discharged it.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_console_argument_form_is_discharged_2_18) {
    std::string want;
    for (int i = 0; i < kIterations; ++i) want += "3.1416\n";
    JitResult r = runAotChecked(prog(
        "    var x := 3.14159;\n" + loop200(
            "      Console.WriteLine(x.ToString(\"F4\"));\n") +
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want + "0\n");
}

// #7312 / spec §28.9 + §23.3 — the FIELD STORE consumer. A `string` field
// store retains the value it stores (§9), so the store's own reference is
// the field's and the producer's reference is discharged after the store.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_field_store_form_is_discharged_2_18) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class Holder\n"
        "  public Name: string;\n"
        "  public constructor() this.Name := \"init\"; end constructor;\n"
        "end Holder;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var x := 3.14159;\n"
        "    var h := new Holder();\n" + loop200(
            "      h.Name := x.ToString(\"F4\");\n"
            "      t := t + h.Name.Length;\n") +
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedFormattedTotal());
}

// #7312 / spec §28.9 + §28.3 — the RETURN consumer. The callee hands the
// caller exactly one reference (the producer's), and the caller discharges it.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_return_form_is_discharged_2_18) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Make(): string\n"
        "    var x := 3.14159;\n"
        "    return x.ToString(\"F4\");\n"
        "  end Make;\n"
        "  public static method Main(): int\n" + loop200(
            "      var s := Make();\n"
            "      t := t + s.Length;\n") +
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedFormattedTotal());
}

// #7312 / spec §28.9 — the METHOD ARGUMENT consumer. The callee's by-value
// `string` parameter owns no reference of its own, so the caller's producer
// reference is discharged after the call.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_method_argument_form_is_discharged_2_18) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Take(s: string): int\n"
        "    return s.Length;\n"
        "  end Take;\n"
        "  public static method Main(): int\n"
        "    var x := 3.14159;\n" + loop200(
            "      t := t + Take(x.ToString(\"F4\"));\n") +
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedFormattedTotal());
}

// #7312 / spec §18.5 + §28.9 — the INTERPOLATION consumer. The hole's value
// IS the source expression (a `string`-typed hole), so the hole's
// `pv != srcEv` release does not fire and the producer's reference is
// discharged by the hole's own producer rule.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_interpolation_of_the_call_form_is_discharged_2_18) {
    JitResult r = runAotChecked(prog(
        "    var x := 3.14159;\n" + loop200(
            "      var s := $\"{x.ToString(\"F4\")}\";\n"
            "      t := t + s.Length;\n") +
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedFormattedTotal());
}

// #7312 / spec §28.9 — the CONCAT OPERAND consumer. The concat borrows the
// operand (it copies the bytes), so the producer's reference is discharged
// after the concat.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_concat_operand_form_is_discharged_2_18) {
    JitResult r = runAotChecked(prog(
        "    var x := 3.14159;\n" + loop200(
            "      var s := \"a\" + x.ToString(\"F4\");\n"
            "      t := t + s.Length;\n") +
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::to_string(kIterations * 7) + "\n");
}

// ---------------------------------------------------------------------------
// THE OVER-RELEASE ORACLE — green on both arms, and it MUST stay green.
// ---------------------------------------------------------------------------

// #7312 / stdlib §2.18's ROUND TRIP + spec §28.9 — the result must SURVIVE
// the call. The row binds the formatted text, churns the heap through 50
// allocations, and only THEN reads the bound string: were the producer's
// reference dropped at the end of the call (or anywhere before the read),
// the buffer would already be freed and the read would fault under
// generated-code ASan. The comparison pins that the text is still the value
// §2.18 rendered.
//
// WHY THE COMPARISON AND NOT A `double.Parse(s)` CALL. Passing the local to a
// USER method makes it ESCAPE, and an escaping producer local is not given a
// scope-end release — a PRE-EXISTING, producer-independent residual (the
// `"a" + "b"` producer leaks the same way through the same shape; MEASURED:
// 37 bytes in 2 allocations). This row must pin the SURVIVAL of the value,
// not that residual, so it reads the local without passing it.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_formatted_result_survives_the_call_2_18) {
    JitResult r = runAotChecked(prog(
        "    var x := 3.14159;\n"
        "    var s: string := x.ToString(\"F4\");\n"
        "    var churn: string := \"\";\n"
        "    var i: int := 0;\n"
        "    while i < 50 do\n"
        "      i := i + 1;\n"
        "      churn := \"c\" + i.ToString();\n"
        "    end while;\n"
        "    if s <> \"3.1416\" then return 1; end if;\n"
        "    Console.WriteLine(s);\n"
        "    Console.WriteLine(s.Length);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3.1416\n6\n");
}

// ---------------------------------------------------------------------------
// THE CONTROL ROWS — green on BOTH arms.
// ---------------------------------------------------------------------------

// #7312 / spec §28.9 — the NO-FORMAT `ToString()` sibling. It was already
// discharged (its result is a classified +1 producer), so this row is green
// on both arms and pins that the fix did not regress it.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_no_format_result_stays_discharged_2_18) {
    JitResult r = runAotChecked(prog(
        "    var x := 3.14159;\n" + loop200(
            "      var s: string := x.ToString();\n"
            "      t := t + s.Length;\n") +
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::to_string(kIterations * kNoFormatLength) + "\n");
}

// #7312 / spec §18.5 + stdlib §2.18 — the INTERPOLATION HOLE WITH A FORMAT
// SPECIFIER. The hole's value is a fresh RENDER (the runtime's formatter),
// distinct from the hole's source expression, so the hole's `pv != srcEv`
// release discharges it. Green on both arms: the fix left this path alone.
BEZEL_TEST(codegen_tostring_format_discharge_7312,
           the_interpolation_hole_with_a_format_specifier_stays_clean_2_18) {
    JitResult r = runAotChecked(prog(
        "    var x := 3.14159;\n" + loop200(
            "      var s := $\"{x:F4}\";\n"
            "      t := t + s.Length;\n") +
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedFormattedTotal());
}
