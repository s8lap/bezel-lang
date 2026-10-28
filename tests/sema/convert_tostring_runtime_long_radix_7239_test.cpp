// Spec-derived regression test for issue #7239.
//
// stdlib §2.3 (v0.345) declares `ToString(value: int, toBase: int): string`
// and `ToString(value: long, toBase: int): string` — the `toBase` parameter
// is declared an `int`.
//
// §5.1 (v0.345): a `long` widens to `float`, `double` and `decimal` — NEVER
// to an `int`. Its constant-narrowing clause is VALUE-based and crosses
// SIGNEDNESS, so a `long` CONSTANT whose value fits `int`'s range IS
// admissible — but a RUN-TIME `long` is not.
//
// BEFORE THE FIX (the issue's own measurement, on the staging tip 40d1691f,
// and reproduced by the fix commit's author on that commit's parent arm): a
// RUN-TIME `long` reached the `Convert.ToString(value, toBase)` lowering as a
// 64-bit value and the renderer narrowed it with an `sext` to the `int` the
// parameter declares — MALFORMED IR, "Type too small for SExt /
// %3 = sext i64 %2 to i32", then "IR verification failed", and the compiler
// ABORTED. The CONSTANT spelling compiled.
//
// THE FIX (picked as `c9dfae6c`, "narrow #7258: an integer's radix rendering
// keeps its value and its width") carries the value into the formatter
// UNCHANGED — the width is the value's own — so no narrowing `sext` is
// emitted and the malformed IR cannot be produced. The `toBase` TYPE is
// checked by sema: a RUN-TIME `long` reaches no `int` parameter (§5.1), so
// the call is a COMPILE ERROR at a real location, while a `long` CONSTANT
// whose value fits `int`'s range is admissible (§5.1's value-based constant
// narrowing, which crosses signedness).
//
// THE ORACLE. `runAot` — the strict AOT path. A rejection row asserts the
// FULL diagnostic at a REAL source location (a row that asserted only "the
// compiler did not abort" would be satisfied by a silent miscompile); the
// control rows assert the RENDERED TEXT.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           decls + "    " + stmt + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// A RUN-TIME `long` at the `toBase` parameter reaches no declared form.
BEZEL_TEST(sema_convert_tostring_runtime_long_radix_7239,
           a_runtime_long_radix_is_rejected) {
    auto r = runAot(prog(
        "    var b: long := 16;\n",
        "Console.WriteLine(Convert.ToString(255, b));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "no matching overload of `Convert.ToString` — stdlib §2.3 declares "
           "`ToString(value: int, toBase: int): string` / "
           "`ToString(value: long, toBase: int): string`, so the `toBase` "
           "argument must be an `int`"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// THE ISSUE'S OWN REPRO. The issue body spells the malformed IR with the
// `long` at the VALUE position and the radix a literal:
//
//   var x: long := 255L;
//   Convert.ToString(value := x, toBase := 16)
//     -> "Type too small for SExt / %3 = sext i64 %2 to i32"
//        "IR verification failed"
//
// stdlib §2.3 declares `ToString(value: long, toBase: int): string`, so the
// `long` VALUE is a declared form's value — the value is 255 and the radix
// text is "ff". The compiler must emit that, not a `sext i64` of the value
// to the `int` the OTHER overload declares.
//
// The row asserts the RENDERED TEXT: a row that asserted only "the compiler
// did not abort" would be satisfied by a program that printed nothing.
BEZEL_TEST(sema_convert_tostring_runtime_long_radix_7239,
           the_issues_own_long_value_repro_renders_its_radix_text) {
    auto r = runAot(prog(
        "    var x: long := 255L;\n",
        "Console.WriteLine(Convert.ToString(value := x, toBase := 16));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}

// CONTROLS: §5.1's CONSTANT narrowing crosses signedness, so a `long`
// CONSTANT whose value fits `int`'s range is WELL-TYPED and must keep
// compiling — and must render the radix form.
BEZEL_TEST(sema_convert_tostring_runtime_long_radix_7239,
           a_long_constant_radix_is_still_admitted) {
    auto r = runAot(prog("", "Console.WriteLine(Convert.ToString(255, 16L));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}

BEZEL_TEST(sema_convert_tostring_runtime_long_radix_7239,
           an_int_constant_radix_is_still_admitted) {
    auto r = runAot(prog("", "Console.WriteLine(Convert.ToString(255, 16));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}
