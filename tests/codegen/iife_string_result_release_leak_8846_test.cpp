// Codegen — issue #8846: the string RESULT of an IMMEDIATELY-INVOKED lambda
// (`(lambda() => t + "q")()`) was never released: 7000 B leaked over 200 calls.
//
// THE RULE. §28.9 (v0.359): `string` values are runtime-managed, every
// reference is reclaimed exactly once. §27.5 (v0.359): a lambda is called the
// same way whether it is bound to a local or invoked in place, so the +1
// string its body mints is owned by the caller either way.
//
// ROOT CAUSE. `var n := lambda() => ...; n()` was recognised as a +1 string
// producer (the callee resolves to a local), so the bound local released it at
// scope end. An IIFE callee is a LambdaExpr, which resolves to no symbol, so
// the same call got no release.
//
// Every program runs its body 200 times under runAotChecked (generated-code
// AddressSanitizer + LeakSanitizer).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string runCleanOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

std::string program(const std::string& runBody) {
    return R"BZL(
module M;
public class Box
  public V: int := 0;
  public constructor(v: int)
    V := v;
  end constructor;
end Box;
public class C
  public static method Take(s: string): int return s.Length; end Take;
  public static method Run(t: string): int
)BZL" + runBody + R"BZL(
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// The issue's repro, as filed. 2 per call.
BEZEL_TEST(iife_string_result_release_leak_8846,
           issue_repro_uncaptured_string_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var r := (lambda() => t + \"q\")(); return r.Length;\n")),
        "400\n");
}

// A parameterised IIFE.
BEZEL_TEST(iife_string_result_release_leak_8846,
           parameterised_iife_string_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var r := (lambda(x: string) => x + \"z\")(t); return r.Length;\n")),
        "400\n");
}

// An IIFE returning a captured string (a borrow the body must retain).
BEZEL_TEST(iife_string_result_release_leak_8846,
           iife_returns_captured_string_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda() => s)(); return r.Length + s.Length;\n")),
        "800\n");
}

// `s + s` over a captured string. 4 + 2 per call.
BEZEL_TEST(iife_string_result_release_leak_8846,
           iife_concatenates_captured_string_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var s := t + \"b\"; var r := (lambda() => s + s)(); return r.Length + s.Length;\n")),
        "1200\n");
}

// The IIFE result passed straight as an argument.
BEZEL_TEST(iife_string_result_release_leak_8846,
           iife_result_as_argument_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    return C.Take((lambda() => t + \"q\")());\n")),
        "400\n");
}

// The IIFE result used as a receiver, in a return.
BEZEL_TEST(iife_string_result_release_leak_8846,
           iife_result_member_in_return_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    return (lambda() => t + \"q\")().Length;\n")),
        "400\n");
}

// The bound IIFE result passed on to a call.
BEZEL_TEST(iife_string_result_release_leak_8846,
           iife_bound_result_passed_on_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var r := (lambda() => t + \"q\")(); return C.Take(r);\n")),
        "400\n");
}

// A class-instance result is unaffected.
BEZEL_TEST(iife_string_result_release_leak_8846,
           iife_class_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var b := (lambda() => new Box(t.Length))(); return b.V;\n")),
        "200\n");
}
