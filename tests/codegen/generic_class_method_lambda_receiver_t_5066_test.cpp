// Regression tests for issue #5066 (§14.1 / §14.5, v0.307): a lambda passed
// to a generic-class instance method whose OWN generic method parameter list
// is non-empty (`Box<T>.Apply<U>(v: T, f: (T) -> U): U`) must have its
// unannotated parameter typed by the RECEIVER's bound class type argument
// (`T` -> `string` for a `Box<string>` receiver), not left as the bare,
// unsubstituted class type parameter.
//
// §14.1 (monomorphization / per-instantiation evaluation): the receiver's
// type arguments bind the class `T`. §14.5 (generic-argument inference):
// phase 2 types a lambda parameter from the bindings established so far —
// here, the RECEIVER's class-level binding, which is available before the
// method's own type parameter `U` is inferred.
//
// Root cause: `formalFnParamType2`'s helper in ExprTypecheck_Exprs.cpp
// (fix #829 / #1140 machinery) skipped substitution entirely whenever the
// callee METHOD had ANY generic parameters of its own — conflating "the
// formal mentions the method's own still-unresolved type parameter" (#505's
// actual hazard) with "the callee happens to be a generic method at all".
// `Apply<U>`'s parameter `f: (T) -> U` has `U` only in the RETURN position;
// its own PARAMETER position (`T`) mentions only the already-bound receiver
// class parameter, so it was safe to substitute — but the blanket gate
// never inspected the parameter's actual generic-name content and bailed
// unconditionally, leaving `y`'s sema type as the bare, unsubstituted `T`.
//
// Effects observed pre-fix (§14.1/§14.5, v0.307's Box<T> repro):
//   - `y.Length` (member access) happened to still evaluate structurally
//     right after #5024's codegen-side receiver-generic-substitution
//     fix landed on origin/main, so it silently printed the RIGHT answer
//     for the wrong reason.
//   - `y + "!"` (a SEMA-level operator check requiring the operand's sema
//     type to literally be `string`) was rejected outright: "`+` requires
//     both operands to be `string`" — this is the row this fix actually
//     changes from reject to accept.
//
// The regression test below pins the STRING-CONCAT row (sema rejection,
// the actual defect this fix repairs) plus the two rows the issue also
// listed for completeness, plus controls: a non-generic method on a
// generic class (must already work, unaffected by this fix), and a static
// generic method (no receiver substitution applies at all).
//
// NOTE: any call through a generic METHOD with its own type parameter (`U`)
// leaks 48 bytes MEASURED IDENTICALLY before and after this fix (confirmed
// by copying the source aside, `git checkout --`, rebuilding, and re-probing
// — see the fail-without notes in this fix's commit) — filed separately as
// #5116, unrelated to lambda parameter typing. Every row that calls such a
// method therefore uses the plain `runAot` (value-only) oracle, not
// `runAotChecked`, so this test does not fail on that pre-existing,
// out-of-scope leak. The non-generic-method control (which does not exercise
// #5116's leak) keeps `runAotChecked` for full memory-cleanliness coverage.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string boxApplyProgram(const std::string& callLine,
                             const std::string& printLine) {
    return std::string(
        "module Q; "
        "public class Box<T> "
        "  public method Apply<U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end Apply; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(); ") +
        callLine + " " + printLine +
        "    return 0; "
        "  end Main; "
        "end Program;";
}

} // namespace

// §14.1 / §14.5 — the issue's third row, and the actual defect this fix
// repairs: sema rejected `y + "!"` because `y`'s type stayed the bare `T`
// instead of the receiver's bound `string`. FAILS pre-fix (compile reject);
// PASSES with the fix, printing "<ab!>".
BEZEL_TEST(codegen_5066, generic_class_instance_method_lambda_param_string_concat_spec_14_1_14_5) {
    auto r = runAot(boxApplyProgram(
        "var s: string := b.Apply<string>(\"ab\", lambda(y) => y + \"!\");",
        "Console.WriteLine($\"<{s}>\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<ab!>\n");
}

// §14.1 / §14.5 — the issue's first row: `y.Length` against `int`.
BEZEL_TEST(codegen_5066, generic_class_instance_method_lambda_param_length_int_spec_14_1_14_5) {
    auto r = runAot(boxApplyProgram(
        "var t: int := b.Apply<int>(\"abc\", lambda(y) => y.Length);",
        "Console.WriteLine($\"<{t}>\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<3>\n");
}

// §14.1 / §14.5 — the issue's second row: `y.Length` against a nullable
// `int?` method type argument.
BEZEL_TEST(codegen_5066, generic_class_instance_method_lambda_param_length_nullable_int_spec_14_1_14_5) {
    auto r = runAot(boxApplyProgram(
        "var s: int? := b.Apply<int?>(\"ab\", lambda(y) => y.Length);",
        "Console.WriteLine($\"<{s}>\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<2>\n");
}

// Control (§14.1) — a NON-generic method on a generic class: the lambda's
// parameter is typed by the receiver's class T with no method-own generic
// parameter in play at all. This path was never gated by `formalFnParamType2`
// and must keep working unchanged.
BEZEL_TEST(codegen_5066, generic_class_nongeneric_method_lambda_param_control_spec_14_1) {
    auto r = runAotChecked(std::string(
        "module Q; "
        "public class Box<T> "
        "  public method ApplyPlain(v: T, f: (T) -> int): int "
        "    return f(v); "
        "  end ApplyPlain; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(); "
        "    var t: int := b.ApplyPlain(\"abcd\", lambda(y) => y.Length); "
        "    Console.WriteLine($\"<{t}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<4>\n");
}

// Control (§14.1 / §14.5) — a STATIC generic method: there is no receiver
// class-level substitution to apply at all (no instance, no class type
// argument), so `formalFnParamType2`'s per-position own-generic-name check
// must still gate correctly on the method's own type parameter alone.
BEZEL_TEST(codegen_5066, static_generic_method_lambda_param_control_spec_14_1_14_5) {
    auto r = runAot(std::string(
        "module Q; "
        "public class Util "
        "  public static method ApplyStatic<T, U>(v: T, f: (T) -> U): U "
        "    return f(v); "
        "  end ApplyStatic; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    var t: int := Util.ApplyStatic<string, int>(\"wxyz\", lambda(y) => y.Length); "
        "    Console.WriteLine($\"<{t}>\"); "
        "    return 0; "
        "  end Main; "
        "end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<4>\n");
}
