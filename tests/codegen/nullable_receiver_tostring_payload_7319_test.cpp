// Spec-derived regression test for issue #7319.
//
// stdlib §9.1 (v0.345) declares the primitive receivers' one-argument
// `ToString(format: string)`, and stdlib §2.18 (v0.345) gives the format
// vocabulary ("`D`/`Dn` decimal (n = min digits, zero-padded)" and
// "`F`/`Fn` fixed-point (n decimals, default 2)").
//
// spec §7.4 (v0.345): a member call on a nullable value is a compile error
// unless the compiler has narrowed it to non-null; spec §22.6 (v0.345): "a
// read of a binding §7.4 has narrowed non-null is of its non-nullable type".
// §7.4's own list carries the DECLARATION form ("from a declaration whose
// initializer is such an `expr` — `var x: T? := expr;` … which is the
// declaration's assignment"), so a local declared `T?` and initialised with a
// non-null value IS narrowed, and the receiver of the call is the PAYLOAD.
//
// So the RECEIVER of the call is the PAYLOAD, never the CARRIER (the tagged
// `{i8 hasValue; T}` struct a nullable value is held in).
//
// BEFORE THE FIX (the issue's own measurement, on the staging tip 2e9c598e,
// and reproduced by the fix commit's author on that commit's parent arm
// 20b96a83): the one-argument arm handed the whole CARRIER to the formatter
// — an `int?` printed the carrier's bytes, and a `double?` reached an `fcmp`
// against a `double` and the compiler ABORTED with malformed IR ("Invalid
// operand types for FCmp instruction").
//
// THE FIX (picked as `eacd9e03`, "narrow #7319: the one-argument ToString arm
// renders a nullable receiver's payload") unwraps the carrier in the
// one-argument arm of `Emitter::lowerExprInner`
// (`src/codegen/LlvmBackend_Exprs.cpp`): `recv = unwrapNarrowedNullableOperand(recv, tt)`.
//
// THE ORACLE. `runAotChecked` — the generated-code AddressSanitizer arm. The
// row that must detect "the compiler did NOT emit malformed IR" asserts the
// COMPILE'S SUCCESS and the program's EXACT stdout TOGETHER: a program that
// compiled but printed nothing, and a program that never compiled, are then
// two different failures — which a stdout-only assertion could not tell
// apart.
//
// WHAT THIS FILE ADDS. The landed fix's own test file
// (`nullable_receiver_tostring_format_7319_test.cpp`) pins the NULL-TEST
// narrowing shape (`if x <> null then …`) for the int, double, float,
// decimal and uint receivers and the radix forms, and the DECLARATION
// narrowing shape for the `int` receiver. The rows here add the
// DECLARATION-narrowing shape for the `double` and the `float` receivers —
// the two carriers the issue names, the `double?` one being the shape that
// carried the COMPILER ABORT — and the UN-narrowed PARAMETER receiver, which
// is the soundness precondition the codegen unwrap depends on (sema's §7.4
// gate rejects member access on an un-narrowed nullable value, so only a
// narrowed read reaches the arm).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A §7.4-DECLARATION-NARROWED nullable receiver: §7.4's list names "a
// declaration whose initializer is such an `expr` — `var x: T? := expr;`",
// so the local is narrowed at its declaration and the call needs no guard.
std::string prog(const std::string& decl, const std::string& call) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + decl + "\n"
           "    Console.WriteLine(" + call + ");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// stdlib §2.18's integer vocabulary: "D5" is five zero-padded decimal
// digits, so the PAYLOAD 5 renders "00005". The CARRIER's bytes would be a
// pointer-sized number, never "00005".
BEZEL_TEST(codegen_nullable_receiver_tostring_payload_7319,
           the_declared_narrowed_int_receiver_renders_the_payload) {
    JitResult r =
        runAotChecked(prog("var x: int? := 5;", "x.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "00005\n");
}

// The DOUBLE receiver is the row that carried the COMPILER ABORT (the
// `{i8, double}` carrier reached an `fcmp` against a `double`).
BEZEL_TEST(codegen_nullable_receiver_tostring_payload_7319,
           the_declared_narrowed_double_receiver_renders_the_payload) {
    JitResult r =
        runAotChecked(prog("var x: double? := 1.5;", "x.ToString(\"F2\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.50\n");
}

// The FLOAT receiver, the third carrier shape the one-argument arm
// mishandled. The `f` suffix is what makes the literal a `float` (§4.2): a
// bare `1.25` is a `double`, and `double` does not convert to `float`
// implicitly (§5.1).
BEZEL_TEST(codegen_nullable_receiver_tostring_payload_7319,
           the_declared_narrowed_float_receiver_renders_the_payload) {
    JitResult r =
        runAotChecked(prog("var x: float? := 1.25f;", "x.ToString(\"F2\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.25\n");
}

// An UN-NARROWED nullable receiver is a compile error (spec §7.4) — the
// arm is only ever reached with a narrowed read. The receiver here is a
// PARAMETER, so §7.4's declaration narrowing cannot have established
// anything for it: a parameter is narrowed only by the forms §7.4 lists, and
// none of them applies at the method's entry.
//
// This row is GREEN ON BOTH ARMS of the fix: it pins the §7.4 REJECTION the
// codegen unwrap depends on (the unwrapper's own precondition is "a SEMA
// GATE already rejects the un-narrowed form"), so a future change that let
// the un-narrowed form through would turn it RED and stop the unwrap from
// being sound.
BEZEL_TEST(codegen_nullable_receiver_tostring_payload_7319,
           the_un_narrowed_nullable_receiver_is_refused) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Take(n: int?): string\n"
        "    return n.ToString(\"D5\");\n"
        "  end Take;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Take(5));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "requires narrowing it to non-null first"));
}
