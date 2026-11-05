// Spec-derived regression for issue #8167: a lambda whose body yields a
// `string?` value the lambda does not itself own -- `lambda() => c.HeadN()`
// over a GENERIC extension member `HeadN(): T?` at T = string, or a captured
// `string?` local -- stored in a `() -> string?` local handed its caller a +0
// borrow. The caller of a function value releases a `string?` result exactly as
// it releases a `string` one, so the second call read freed memory
// (AddressSanitizer: heap-use-after-free).
//
// THE RULES (spec v0.355):
//   §28.9 "Function-typed values" and `string` are the runtime-managed tier: a
//   string handed out of a call is a value the caller owns for the call, and
//   reading it after the call must not touch freed memory.
//   §28.3 "Local variables, ownership, and borrows": a returned borrow is made
//   an independent reference (the callee mints) -- the same rule for a
//   `string?` return as for a `string` one (isPlusOneStringProducer's
//   closure-call arm admits "nullable OR non-nullable", #2196).
//   §27.3 (lambda expressions): a lambda's body, expression or block, yields
//   the lambda's result.
//
// THE DEFECT. The lambda mint (LlvmBackend_Exprs2.cpp expression body, and its
// block-body twin in emitReturn, LlvmBackend_Stmts.cpp) was gated on a
// NON-nullable `string` result, while the call site of a function value treats
// a `string?` result as a +1 it releases. The inlined generic-block member
// mints nothing itself, so nothing balanced the release.
//
// Every row runs the lambda 100 times under the checked oracle (ASan + LSan),
// reads the string's LENGTH (so a freed or wrong string changes stdout), and
// asserts the exact stdout. The `non-generic` and `non-nullable` rows are the
// issue's own controls and are green on the base (positive controls).

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

JitResult runChecked(const std::string& p) {
    JitResult r = runAotChecked(p);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    return r;
}

// Main's tail: call `f` 100 times, summing the result's length.
const char* kLoop =
    "    var tot := 0; "
    "    var i := 0; "
    "    while i < 100 do "
    "      var s1 := f(); "
    "      tot := tot + (s1 ?? \"\").Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(tot); "
    "    return 0; "
    "  end Main; "
    "end C;";

// A generic CLASS receiver: its local does not leak one-shot on the base the way
// a captured `Array<string>` local does (open #6516 / #7830), so the checked
// oracle's leak half stays meaningful. The Array spelling of the issue's repro
// is the last row.
const char* kBoxExt =
    "class Box<T>(item: T) "
    "  public property Item: T => item; "
    "end Box; "
    "extension Box<T> "
    "  public method HeadN(): T? => this.Item; "
    "  public method Head(): T => this.Item; "
    "end; ";
const char* kBoxSetup =
    "using var b := new Box<string>(\"w\" + n.ToString()); ";
// A class whose property reads a `string?` borrow of its own field.
const char* kHolder =
    "class H(s: string?) "
    "  public property Name: string? => s; "
    "end H; ";
const char* kHolderSetup =
    "using var h := new H(\"w\" + n.ToString()); ";

std::string prog(const std::string& decls, const std::string& setup,
                 const std::string& fDecl) {
    return "module M; import System; " + decls +
           "class C "
           "  public static method Main(): int "
           "    var n := 41; " +
           setup + fDecl + kLoop;
}

}  // namespace

// A generic-block extension member returning `T?` at T = string, in an
// expression-bodied lambda stored in a local (the issue's mechanism).
BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           expr_lambda_generic_ext_nullable_string_stored_spec_28_9) {
    JitResult r = runChecked(prog(
        kBoxExt, kBoxSetup,
        "var f: () -> string? := lambda() => b.HeadN(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// The block-bodied twin: emitReturn's lambda arm had the same gate.
BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           block_lambda_generic_ext_nullable_string_stored_spec_28_9) {
    JitResult r = runChecked(prog(
        kBoxExt, kBoxSetup,
        "var f: () -> string? := lambda(): string? => do "
        "  return b.HeadN(); end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// A `string?` property read (a borrow of the object's field): the same +0
// result, no generic member involved.
BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           expr_lambda_nullable_string_property_read_spec_28_9) {
    JitResult r = runChecked(prog(
        kHolder, kHolderSetup,
        "var f: () -> string? := lambda() => h.Name; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           block_lambda_nullable_string_property_read_spec_28_9) {
    JitResult r = runChecked(prog(
        kHolder, kHolderSetup,
        "var f: () -> string? := lambda(): string? => do "
        "  return h.Name; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// A null result stays a null carry: the mint is a no-op on null.
BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           expr_lambda_null_string_property_stays_null_spec_28_9) {
    JitResult r = runChecked(prog(
        "class H(s: string?) "
        "  public property Name: string? => s; "
        "end H; ",
        "using var h := new H(null); ",
        "var f: () -> string? := lambda() => h.Name; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// The lambda passed DIRECTLY (not stored) to a function-typed parameter.
BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           expr_lambda_generic_ext_nullable_string_passed_directly_spec_28_9) {
    JitResult r = runChecked(
        "module M; import System; " + std::string(kBoxExt) +
        "class C "
        "  static method Run(g: () -> string?): int "
        "    var tot := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var s1 := g(); "
        "      tot := tot + (s1 ?? \"\").Length; "
        "      i := i + 1; "
        "    end; "
        "    return tot; "
        "  end Run; "
        "  public static method Main(): int "
        "    var n := 41; " +
        std::string(kBoxSetup) +
        "    Console.WriteLine(Run(lambda() => b.HeadN())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// Controls (green on the base): a non-nullable generic result, a non-generic
// extension member, and a lambda whose body is a +1 producer (the transfer
// exclusion: no second retain, so no leak).
BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           control_generic_extension_non_nullable_result_spec_28_9) {
    JitResult r = runChecked(prog(
        kBoxExt, kBoxSetup,
        "var f: () -> string := lambda() => b.Head(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           control_non_generic_extension_nullable_result_spec_28_9) {
    JitResult r = runChecked(prog(
        "class Bx(item: string) "
        "  public property Item: string => item; "
        "end Bx; "
        "extension Bx "
        "  public method NgHeadN(): string? => this.Item; "
        "end; ",
        "using var b := new Bx(\"w\" + n.ToString()); ",
        "var f: () -> string? := lambda() => b.NgHeadN(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

BEZEL_TEST(lambda_generic_ext_nullable_result_8167,
           control_expr_lambda_nullable_string_plus_one_producer_spec_28_9) {
    JitResult r = runChecked(prog(
        kHolder, kHolderSetup,
        "var f: () -> string? := lambda() => (h.Name ?? \"\") + \"\"; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}
