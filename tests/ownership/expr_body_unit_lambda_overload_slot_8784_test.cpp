// fix #8784 (§22.6 *Implicit conversions and tie-breaking* / *Lambda-literal
// arguments*, §27.3 *Expression body*, §22.7, v0.359):
//
//   "A lambda literal has no static type of its own; applicability and ranking
//    at that argument position are decided by its **inferred return type**. ...
//    the candidate is applicable at the position when the lambda's inferred
//    return type equals the candidate's return slot, implicitly converts to it
//    (§5.1), or binds it"
//
//   "Expression-bodied lambdas need no inference rule: the body expression's
//    type is the return type, and a body of type `()` makes a procedure
//    lambda." (§27.3)
//
//   "A procedure-bodied lambda does not convert to a value-returning function
//    type." (§27.3)
//
//   "The rule applies identically to constructor overload sets (§22.7): a
//    `new T(...)` call is resolved among `T`'s constructors by the same
//    exact-match-then-ambiguity precedence." (§22.6)
//
// THE DEFECT. Two constructors that differ only in the RETURN TYPE of a
// function-typed parameter — `K(f: () -> string)` beside `K(f: () -> ())` —
// overload-resolve on the lambda's INFERRED RETURN TYPE. A lambda whose body
// is of type `()` is a PROCEDURE lambda, so it is exact at the `() -> ()`
// slot and INAPPLICABLE at the `() -> string` slot; the `() -> ()` constructor
// is the only applicable candidate and must win.
//
// MEASURED on r30-c1 @ 983559b09 (the base of this fix), with
// `bezelc --sanitize=address`:
//
//   `new K(lambda() => Console.WriteLine("x"))` against
//   `K(f: () -> string)` (whose body CALLS `f()`) beside `K(f: () -> ())`
//     -> AddressSanitizer:DEADLYSIGNAL ... SEGV on unknown address 0x200d
//
// The overload resolution ignored the function-typed parameter's RETURN type
// and lowered the FIRST-DECLARED candidate; the `() -> string` slot then
// called a `()`-returning body through a VALUE-returning function pointer and
// read a garbage pointer. The METHOD twin (`M(f: () -> string)` beside
// `M(f: () -> ())`) SEGV'd the same way.
//
// THE ROOT CAUSE. §27.3 says an expression body's type IS the lambda's return
// type, and a body of type `()` makes a procedure lambda — but sema leaves a
// unit-returning BUILT-IN member call (`Console.WriteLine`, `Console.Write`,
// `Array<T>.Add`) UNTYPEED on purpose ("Write / WriteLine -> unit (leave
// untyped)"), so the body's recorded type was MISSING where the spec says it
// is `()`. The lambda's inferred return type was missing too, and every reader
// of the lambda's function type then saw "unknown" where the spec says `()`:
// the §22.6 overload grid (`gradeLambdaLiteralCell`) left the cell UNGRADED,
// the #6243 certain-evidence filter (`procedureLambdaInapplicableAt`) did not
// rule out a value-returning slot, and the #6268 deferred §27.3 check had
// nothing to re-judge.
//
// This is the EXPRESSION-bodied twin of the hole #6643 recorded and closed for
// the single-candidate path ("Its predicate excludes `exprBody`, and so does
// the #6268 deferred check"), and of the BLOCK-bodied twin #6268 closed for
// the overload path. Only the expression body in an OVERLOAD set was left —
// filed separately as #8821, whose two halves (the unit-typed expression body
// and the `: ()` ascription) this change closes together.
//
// THE FIX. The lambda's inferred return type is `()` where the body is of type
// `()` (§27.3), so the §22.6 grid grades the cell (exact at `() -> ()`,
// inapplicable at `() -> string`), the #6243 filter rules a value-returning
// slot out, and the #6268 deferred check re-judges the chosen candidate's
// formal. The `: ()` ascription half of #8821 comes with it: an ascribed
// procedure lambda's recorded return is `()` too, and the same filter then
// rules the value-returning slot out for it.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output AND that neither AddressSanitizer nor
// LeakSanitizer fired, so the garbage-pointer read cannot pass as a run.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// Neither AddressSanitizer nor LeakSanitizer fired: no SEGV report, no leak
// summary. LeakSanitizer prints a SUMMARY line only when it finds a leak, so
// "no summary" is 0.
void assertNoSanitizerReport(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("AddressSanitizer:DEADLYSIGNAL") ==
                      std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran (a
// toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoSanitizerReport(r);
}

// The §27.3 verdict an expression body of type `()` earns at a value-returning
// slot — the same wording the single-candidate path uses (#6643).
#define UNIT_BODY_MSG                                                        \
    "a procedure-bodied lambda (a body of type `()`) does not convert to a " \
    "function type whose return type is a value"

// The unit body: an expression-bodied lambda whose body is a unit-returning
// built-in call. `Console.WriteLine` is deliberately left untyped by sema, so
// the body's type is MISSING where §27.3 says it is `()`.
const char* const kUnitBody = "lambda() => Console.WriteLine(\"x\")";

}  // namespace

// THE REPRO (constructor overload set). The two constructors differ only in
// the RETURN TYPE of the function-typed parameter. The unit body is EXACT at
// the `() -> ()` slot and INAPPLICABLE at the `() -> string` slot (§22.6 *
// Lambda-literal arguments* + §27.3), so the `() -> ()` constructor is the
// only applicable candidate and must win.
//
// RED on the base: the first-declared `() -> string` constructor was lowered
// and its `f()` call read a garbage pointer — AddressSanitizer:DEADLYSIGNAL
// (SEGV). GREEN with the fix: the `() -> ()` constructor runs and prints "B".
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           constructor_return_slot_selects_the_procedure_constructor_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> string) Console.WriteLine(\"A:\" + f()); end;\n"
        "  public constructor(f: () -> ()) Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(" + std::string(kUnitBody) + ");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "B\n");
}

// THE METHOD TWIN of the repro: the same overload set on a method instead of
// a constructor. §22.6's rule applies identically to a method overload set.
//
// RED on the base: the same SEGV. GREEN with the fix: "B".
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           method_return_slot_selects_the_procedure_overload_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public static class K\n"
        "  public static method M(f: () -> string) Console.WriteLine(\"A:\" + f()); end;\n"
        "  public static method M(f: () -> ()) Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    K.M(" + std::string(kUnitBody) + ");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "B\n");
}

// The same overload set with the VALUE-returning constructor DECLARED FIRST —
// the order in which the base's first-declared pick is the WRONG one. The
// resolution must not depend on the declaration order (§22.6): the unit body
// is inapplicable at the `() -> int` slot whatever order the candidates are
// declared in.
//
// RED on the base: the `() -> int` constructor was lowered and its `f()` call
// read a garbage integer (SEGV). GREEN with the fix: "B".
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           value_returning_constructor_declared_first_is_inapplicable_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> int) Console.WriteLine(\"A:\" + f().ToString()); end;\n"
        "  public constructor(f: () -> ()) Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(" + std::string(kUnitBody) + ");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "B\n");
}

// EVERY candidate value-returning (`() -> string` beside `() -> int`): no
// candidate is applicable — the unit body converts to neither — so the call
// is a compile error (§22.6: "If no candidate is applicable at all ... the
// call is a compile error with no candidates cited"), reported by §27.3's own
// rule ("a procedure-bodied lambda does not convert to a value-returning
// function type"). It must NOT compile and crash.
// RED on the base: the first-declared `() -> string` constructor was lowered
// and its `f()` call read a garbage pointer (SEGV at RUN time, so the
// compile-time rejection the row demands never happened). GREEN with the fix:
// the rejection, at compile time.
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           every_candidate_value_returning_is_rejected_not_crashed_spec_27_3) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> string) Console.WriteLine(\"A:\" + f()); end;\n"
        "  public constructor(f: () -> int) Console.WriteLine(\"B:\" + f().ToString()); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(" + std::string(kUnitBody) + ");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, UNIT_BODY_MSG));
    assertNoSanitizerReport(r);
}

// #8821's own repro: the `() -> int` constructor declared FIRST, and the
// `() -> ()` constructor's body CALLS the lambda — so the procedure candidate
// is not merely selected, it RUNS the unit body. The unit body is inapplicable
// at the `() -> int` slot (§22.6 + §27.3), so the `() -> ()` constructor is
// the only applicable candidate.
//
// RED on the base: the `() -> int` constructor ran (it printed "A" and never
// called the body). GREEN with the fix: "B" then the body's own output.
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           procedure_constructor_runs_the_unit_body_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> int) Console.WriteLine(\"A\"); end;\n"
        "  public constructor(f: () -> ()) do Console.WriteLine(\"B\"); f(); end; end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(" + std::string(kUnitBody) + ");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "B\nx\n");
}

// #8821's `: ()` ASCRIPTION half: an explicitly ascribed procedure lambda
// (`lambda(): () => ...`) is a procedure lambda by §27.3 ("A unit ascription
// (`: ()`) declares a procedure lambda"), so the `() -> int` candidate is
// inapplicable there too and the `() -> ()` one must run.
//
// RED on the base: the `() -> int` constructor ran. GREEN with the fix: "B"
// then the body's own output.
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           unit_ascription_selects_the_procedure_constructor_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> int) Console.WriteLine(\"A\"); end;\n"
        "  public constructor(f: () -> ()) do Console.WriteLine(\"B\"); f(); end; end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(lambda(): () => Console.WriteLine(\"body\"));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "B\nbody\n");
}

// #8821's METHOD-PATH twin: the same overload set on a method instead of a
// constructor. §22.6's rule applies identically to a method overload set.
//
// RED on the base: the `() -> int` overload ran. GREEN with the fix: "B" then
// the body's own output.
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           procedure_overload_runs_the_unit_body_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public static class K\n"
        "  public static method M(f: () -> int) Console.WriteLine(\"A\"); end;\n"
        "  public static method M(f: () -> ()) do Console.WriteLine(\"B\"); f(); end; end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    K.M(" + std::string(kUnitBody) + ");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "B\nx\n");
}

// GREEN CONTROL (green on the base too). The issue's own repro — the
// `() -> string` / `() -> int` constructor pair with a VALUE lambda — already
// resolves to the `() -> int` constructor on the base (#6642's redo), and this
// fix must not disturb it: the lambda's inferred return type is `int`, which
// is exact at `() -> int` and inapplicable at `() -> string` (§22.6).
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           value_lambda_constructor_pair_still_selects_the_int_constructor_control_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> string) Console.WriteLine(\"A\"); end;\n"
        "  public constructor(f: () -> int) Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(lambda() => 1);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "B\n");
}

// GREEN CONTROL (green on the base too). A unit body at a `() -> ()` slot is
// EXACT there (§22.6 *Lambda-literal arguments*: "a unit-typed body ... is
// exact"), so the procedure overload is the one that runs — and the value
// overload beside it is inapplicable, not ambiguous. The base reaches the same
// answer here only by declaring order; the fix reaches it by the rule.
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           unit_body_at_procedure_slot_is_exact_control_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class K\n"
        "  public constructor(f: () -> ()) Console.WriteLine(\"A\"); end;\n"
        "  public constructor(f: () -> int) Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var k := new K(" + std::string(kUnitBody) + ");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "A\n");
}

// ---------------------------------------------------------------------------
// FIX-FORWARD rows (review r43-rev-m69-8784, REJECT of 1572c2794).
//
// §27.3: "the body expression's type is the return type, and a body of type
// `()` makes a procedure lambda." The first cut read "the body's recorded type
// is MISSING and the call's callee has no recorded type" as "the body is of
// type `()`". That holds for a built-in unit call (`Console.WriteLine`) whose
// receiver RESOLVED, but ALSO for ANY call on a lambda parameter whose type is
// still a method TYPE PARAMETER `T` (sema cannot look the member up yet), and
// those bodies return values. The guess went into the lambda's inferred return
// type and into the §22.6 grid, so a `(T) -> string` overload lost to the
// `(T) -> ()` one (a wrong pick the base got right), and `Map<T, U>`'s `U` was
// inferred as `()` (a silent miscompile on a program the base rejects).
// ---------------------------------------------------------------------------

// §22.6 *Lambda-literal arguments*: `x.ToString()` is a VALUE body of type
// `string`, so it is exact at the `(T) -> string` slot; it must never be
// classified as a body of type `()`.
//
// RED on 1572c2794: prints "B" (the string result discarded). GREEN: "A:5".
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           type_parameter_receiver_string_body_selects_the_value_overload_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public static class K\n"
        "  public static method M<T>(x: T, f: (T) -> string) Console.WriteLine(\"A:\" + f(x)); end;\n"
        "  public static method M<T>(x: T, f: (T) -> ()) Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    K.M(5, lambda(x) => x.ToString());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "A:5\n");
}

// The same shape where the lambda parameter is a USER class and the body calls
// a user method returning `string`.
//
// RED on 1572c2794: prints "B". GREEN: "A".
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           type_parameter_receiver_user_class_body_selects_the_value_overload_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public class Item\n"
        "  public method Name(): string => \"n\";\n"
        "end;\n"
        "public static class K\n"
        "  public static method M<T>(x: T, f: (T) -> string) Console.WriteLine(\"A\"); end;\n"
        "  public static method M<T>(x: T, f: (T) -> ()) Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    K.M(new Item(), lambda(x) => x.Name());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "A\n");
}

// The same shape under a constraint (`where T: IFoo`) with an `int` body.
//
// RED on 1572c2794: prints "B". GREEN: "A".
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           constrained_type_parameter_receiver_int_body_selects_the_value_overload_spec_22_6) {
    auto r = runAotChecked(
        "module T;\n"
        "public interface IFoo\n"
        "  method Get(): int;\n"
        "end;\n"
        "public static class K\n"
        "  public static method M<T>(x: T, f: (T) -> int) where T: IFoo Console.WriteLine(\"A\"); end;\n"
        "  public static method M<T>(x: T, f: (T) -> ()) where T: IFoo Console.WriteLine(\"B\"); end;\n"
        "end;\n"
        "public class Foo : IFoo\n"
        "  public method Get(): int => 1;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    K.M(new Foo(), lambda(x) => x.Get());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    expectClean(r, 0, "A\n");
}

// §14.5 / §27.3: a generic `Map<T, U>(x: T, f: (T) -> U): U` over a lambda
// whose body is a VALUE call on a `T`-typed parameter. The body is not of type
// `()`, so `U` must never be inferred as `()`: the program either compiles and
// prints the body's value `7`, or is rejected with the "cannot infer" error —
// it must NEVER compile and print `()`.
//
// RED on 1572c2794: compiled, exit 0, printed "()" (a silent miscompile; the
// base rejects with "cannot infer the type argument `U`"). GREEN: the base's
// rejection (whether a `T`-typed lambda parameter should infer `U` here is a
// separate, pre-existing question; this row pins only that `()` is not it).
BEZEL_TEST(expr_body_unit_lambda_overload_slot_8784,
           type_parameter_receiver_value_body_never_infers_unit_return_spec_27_3) {
    auto r = runAotChecked(
        "module T;\n"
        "public interface IFoo\n"
        "  method Get(): int;\n"
        "end;\n"
        "public class Foo : IFoo\n"
        "  public method Get(): int => 7;\n"
        "end;\n"
        "public static class K\n"
        "  public static method Map<T, U>(x: T, f: (T) -> U): U => f(x);\n"
        "  public static method Run<T>(x: T) where T: IFoo\n"
        "    var r := K.Map(x, lambda(y) => y.Get());\n"
        "    Console.WriteLine(r);\n"
        "  end;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    K.Run(new Foo());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    if (r.ran) {
        expectClean(r, 0, "7\n");
    } else {
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot infer"));
    }
}
