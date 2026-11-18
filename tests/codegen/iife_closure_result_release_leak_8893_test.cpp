// Codegen — issues #8893 / #8870: the CLOSURE result of an IMMEDIATELY-INVOKED
// lambda (`(lambda(): () -> int => lambda() => 3)()`) was never released: the
// closure cell the IIFE's body minted leaked on every call (64 bytes).
//
// THE RULE. §28.9 (v0.359): a function-typed value is runtime-managed — "the
// type's lifetime is managed by the runtime without user involvement" — and
// runtime management governs "the heap-allocated closure object that backs
// it", so that object is reclaimed exactly once. §27.5 (v0.359): "Captured
// variables are heap-allocated in a closure object" — the closure object is a
// heap object, and a lambda's body mints it whoever the caller is.
//
// ROOT CAUSE. `var g := lambda(): () -> int => lambda() => 3; var f := g();`
// was recognised as a +1 closure producer (the callee resolves to a local), so
// the bound local released the closure cell at scope end. An IIFE callee is a
// LambdaExpr, which resolves to no symbol, so the very same call got no
// release — and the +1 the IIFE's body minted was never dropped.
//
// MEASURED on the base (r30-c1 @ e1bbf3711, `bezelc --sanitize=address`):
//   var f: () -> int := (lambda(): () -> int => lambda() => 3)();
//     -> "SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s)."
//   var g: () -> () -> int := lambda(): () -> int => lambda() => 3;
//   var f: () -> int := g();          <- the NAMED twin: CLEAN.
//
// Every program runs its body 200 times under runAotChecked (generated-code
// AddressSanitizer + LeakSanitizer).
//
// NOT a row: the IIFE's closure result CAPTURING a local of the enclosing
// scope. After this fix the closure cell and its env are reclaimed, but the
// CAPTURED local's boxed cell is not (the NAMED twin of the same shape leaks
// nothing) — a separate defect, filed as #8922. A row must be clean after the
// fix, so that shape cannot be one. See the note at the end of this file.

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

// A class with one static method `Mk` that returns a closure (the shape the
// "returned by a method" row needs) and the `Run` method the body under test
// lives in.
std::string program(const std::string& runBody) {
    return R"BZL(
module M;
public class D
  public constructor() end constructor;
  public method Hi(): int return 7; end Hi;
end D;
public class C
  public static method Mk(): () -> int
    return (lambda(): () -> int => lambda() => 3)();
  end Mk;
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

// The issue's repro, as filed (#8893). 3 per call.
BEZEL_TEST(iife_closure_result_release_leak_8893,
           issue_repro_bound_closure_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var f: () -> int := (lambda(): () -> int => lambda() => 3)();"
        " return f();\n")),
        "600\n");
}

// The IIFE's closure result DISCARDED in statement position.
BEZEL_TEST(iife_closure_result_release_leak_8893,
           discarded_closure_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    (lambda(): () -> int => lambda() => 3)();\n    return 1;\n")),
        "200\n");
}

// The §17.13 discard spelling `_ := …`.
BEZEL_TEST(iife_closure_result_release_leak_8893,
           discard_statement_closure_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    _ := (lambda(): () -> int => lambda() => 3)();\n    return 1;\n")),
        "200\n");
}

// The IIFE's closure result handed to the console (an `Object` sink that
// borrows the value — the caller keeps the reference and must drop it).
BEZEL_TEST(iife_closure_result_release_leak_8893,
           console_sink_closure_result_spec_28_9) {
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "() -> int\n";
    expected += "200\n";
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    Console.WriteLine((lambda(): () -> int => lambda() => 3)());\n"
        "    return 1;\n")),
        expected);
}

// The IIFE's result is a METHOD REFERENCE — a closure cell too. 7 per call.
BEZEL_TEST(iife_closure_result_release_leak_8893,
           method_reference_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var d := new D();\n"
        "    var r := (lambda() => d.Hi)();\n"
        "    return r();\n")),
        "1400\n");
}

// The IIFE's lambda has NO return-type ascription: the result type is
// INFERRED from the body. 3 per call.
BEZEL_TEST(iife_closure_result_release_leak_8893,
           inferred_return_type_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var f := (lambda() => lambda() => 3)();\n    return f();\n")),
        "600\n");
}

// The IIFE's lambda has a BLOCK body — a different return path than the
// expression body. 3 per call.
BEZEL_TEST(iife_closure_result_release_leak_8893,
           block_body_closure_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var f: () -> int :="
        " (lambda(): () -> int => do return lambda() => 3; end)();\n"
        "    return f();\n")),
        "600\n");
}

// The IIFE's closure result is RETURNED by a method — the return site must
// transfer the +1 instead of minting a second one. 3 per call.
BEZEL_TEST(iife_closure_result_release_leak_8893,
           returned_closure_result_spec_28_9) {
    BEZEL_EXPECT_EQ(runCleanOut(program(
        "    var f := C.Mk();\n    return f();\n")),
        "600\n");
}

// NOT a row: the IIFE's closure result CAPTURING a local of the enclosing
// scope. MEASURED (r30-c1 @ e1bbf3711, `bezelc --sanitize=address`):
//
//   var k := 10;
//   var f := (lambda(): () -> int => lambda() => k + 3)();
//
// leaks 76 B / 3 allocations on the base and 4 B / 1 allocation after this
// fix — the closure cell and its env are reclaimed, but the CAPTURED local's
// boxed cell is not, and the NAMED twin of the same shape leaks nothing. That
// residual is a separate defect (the capture cell's owner route), filed as
// #8922; it is not this issue's closure-result release, so it cannot be a row
// here (a row must be clean after the fix).

