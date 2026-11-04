// Spec-derived regression tests for issue #8134 — §22.15 *Pure members*
// ("The pure body"), §17.8 (the null-forgiving operator), §A.7 (the `case`
// forms), and §31.4 *Standard exception types* (language spec v0.355).
//
// THE DEFECT — three numbered items, all in `src/sema/PureChecker.cpp`'s
// `PureBodyWalker`.
//
// 1. The OPERAND of the postfix null-forgiving operator (`x!`, §17.8) was
//    never walked. §22.15's list is closed — "A pure body may contain
//    exactly the following; the list is closed, and everything outside it is
//    rejected with a diagnostic naming the operation and this section" — so
//    an impure call inside the operand of a `!` was silently admitted:
//    `Console.ReadLine()!.Length` compiled in a `pure` body, and §22.15
//    rule 6 ("a call … is permitted iff the member declared on the
//    receiver's static type … is `pure`-declared or pure-classified") never
//    fired.
//
// 2. The ARMS of a `case` EXPRESSION (§A.7, `CaseExpr`) were never walked
//    either: neither the arm's `when` GUARD nor its body. The
//    statement-position twin (`CaseStmt`) walked the arms' statement bodies
//    but not their guards. `(case x of 0 => Console.ReadLine(), _ => "")`
//    compiled in a `pure` body.
//
// 3. The §22.15 THROW CARVE-OUT did not check WHICH constructor runs. It
//    admitted `new E(...)` at the throw operand when E had a `pure`
//    constructor SOMEWHERE — so a class with a `pure` one-argument
//    constructor and an impure two-argument one admitted `throw new E("m",
//    cause)`, which runs the impure constructor — and it admitted every
//    built-in exception construction, including §31.4's `(message, cause)`
//    chaining constructor, which §31.4 states is NOT pure.
//
// FIX-FORWARD (the review of item 3): the item-3 rows below introduced an
// OVER-REJECTION — a `pure` constructor with a DEFAULTED parameter, invoked
// with the default omitted, was refused ("the invoked constructor of
// `MyError` could not be identified"), because the typecheck pass publishes
// its constructor selection only in its OVERLOAD arms, and the carve-out's
// fallback then matched an EXACT parameter count that a call leaving a
// default out cannot satisfy. The rows at the END of this file pin the fix:
// the single-constructor case (local and IMPORTED) is admitted again, and
// the overloaded case still answers about the constructor the call INVOKES.
//
// THE ORACLE. Two, deliberately.
//
//   * The SEMA oracle (`parseAndCheckPureGated`) is the phase under test: it
//     runs lex + parse + symbols + types + expressions + the §22.15
//     pure-body checker, gated exactly as the driver gates them. Each REJECT
//     row asserts the EXACT diagnostic text and the LINE the diagnostic
//     lands on, so a row cannot pass by observing the absence of something.
//   * The AOT oracle (`runAot`) is the strict whole-program oracle: the real
//     `Driver::compileFile`, forked, then clang-link and native exec. Each
//     shape has one AOT row proving the driver agrees — the REJECT rows that
//     the program is refused with the same diagnostic, the ADMIT rows that
//     the program COMPILES, LINKS and RUNS (exit code 0), so "it compiled" is
//     never the whole assertion.
//
// The sources are written with real newlines so each rejected row pins the
// diagnostic to its own line.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

namespace {

// The SEMA oracle — the pure-body checker under test.
bezel::test::SemaResult pureCheck(std::string_view src) {
    return bezel::test::parseAndCheckPureGated(src);
}

// The diagnostics whose message contains `needle`.
std::vector<const bezel::Diagnostic*> diagnosticsContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    std::vector<const bezel::Diagnostic*> out;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) out.push_back(&d);
    return out;
}

// The ONE diagnostic whose message contains `needle` (asserts there is
// EXACTLY one, so a row cannot pass on a stray diagnostic that happens to
// carry the same words).
const bezel::Diagnostic* theDiagnosticContaining(
    const bezel::test::SemaResult& r, const std::string& needle) {
    auto hits = diagnosticsContaining(r, needle);
    if (hits.size() != 1) return nullptr;
    return hits.front();
}

// The AOT oracle — the real driver, forked. On a compile reject the
// driver's stderr lands in ONE Error diagnostic, so the needle is matched
// against that text.
bool aotRejectsWith(std::string_view src, const std::string& needle) {
    auto r = bezel::test::runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The AOT oracle's ADMIT side: the program must compile, LINK and RUN, and
// exit 0. `r.ran` false would mean the compile or the link failed, so the
// exit-code assertion alone is not enough.
bool aotRunsWithExitZero(std::string_view src) {
    auto r = bezel::test::runAot(src);
    return r.ran && r.exitCode == 0;
}

// §22.15 rule 6's diagnostic for a USER member that is not `pure`.
constexpr const char* kUserImpureCall =
    "the resolved target `Feed` is not pure-declared";
// §22.15 rule 6's diagnostic for a STANDARD-LIBRARY member the library does
// not mark `pure`.
constexpr const char* kStdlibImpureCall =
    "the standard library does not declare `Console.ReadLine` pure";
// §22.15's throw carve-out's own diagnostic.
constexpr const char* kCarveOut =
    "class construction at `throw` is not admitted in a pure body";

}  // namespace

// =========================================================================
// ITEM 1 — the postfix `!` operand (§17.8 / §22.15).
// =========================================================================

// §22.15's list is closed, so the operand of a `!` is a sub-expression of
// the pure body and rule 6 governs the call inside it. A USER method that
// is not `pure` is rejected, and the diagnostic lands on the CALL.
BEZEL_TEST(pure_body_walk_8134,
           postfix_bang_operand_user_impure_call_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static method Feed(): string\n"
        "    return \"x\";\n"
        "  end Feed;\n"
        "end Helper;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    return Helper.Feed()!;\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 9u);
    // The `!` itself is NOT the reported operation (§17.8: the operator is
    // "purely a type-system assertion") — the CALL inside its operand is.
    BEZEL_EXPECT_TRUE(
        diagnosticsContaining(r, "null-forgiving").empty());
}

// The STANDARD-LIBRARY twin: `Console.ReadLine()` is not `pure`-declared in
// the standard library, so the call inside the `!` operand is rule 6's
// business.
BEZEL_TEST(pure_body_walk_8134,
           postfix_bang_operand_stdlib_impure_call_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(): int\n"
        "    return Console.ReadLine()!.Length;\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kStdlibImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
}

// The ADMIT control: the same position with a `pure`-declared callee. The
// `!` operator itself is admitted — §17.8: "The operator is purely a
// type-system assertion (with a runtime null check); it does not move, copy,
// or borrow beyond what reading `x` already does."
BEZEL_TEST(pure_body_walk_8134,
           postfix_bang_operand_pure_call_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static pure method Feed(): string?\n"
        "    return null;\n"
        "  end Feed;\n"
        "end Helper;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    return Helper.Feed()!;\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of item 1: the real driver refuses the program.
BEZEL_TEST(pure_body_walk_8134,
           postfix_bang_operand_impure_call_rejected_by_driver_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class Helper\n"
        "  public static method Feed(): string\n"
        "    return \"x\";\n"
        "  end Feed;\n"
        "end Helper;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    return Helper.Feed()!;\n"
        "  end F;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(C.F());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kUserImpureCall));
}

// =========================================================================
// ITEM 2 — the arms of a `case` EXPRESSION (§A.7 / §22.15).
// =========================================================================

// The arm's BODY is a sub-expression of the pure body. A USER method that
// is not `pure` is rejected there.
BEZEL_TEST(pure_body_walk_8134,
           case_expression_arm_user_impure_call_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static method Feed(): string\n"
        "    return \"x\";\n"
        "  end Feed;\n"
        "end Helper;\n"
        "class C\n"
        "  public static pure method F(x: int): string\n"
        "    return (case x of 0 => Helper.Feed(), _ => \"\");\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kUserImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 9u);
}

// The STANDARD-LIBRARY twin, in the arm's body.
BEZEL_TEST(pure_body_walk_8134,
           case_expression_arm_stdlib_impure_call_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(x: int): string?\n"
        "    return (case x of 0 => Console.ReadLine(), _ => null);\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kStdlibImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
}

// The arm's `when` GUARD is an arm's own sub-expression (§19.4), and it is
// subject to the same closed list.
BEZEL_TEST(pure_body_walk_8134,
           case_expression_guard_stdlib_impure_call_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(x: int): string\n"
        "    return (case x of 0 when Console.ReadLine()!.Length > 0 => \"y\", "
        "_ => \"n\");\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kStdlibImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
}

// The statement-position twin shares the `ast::CaseArm` shape, so its guard
// is now walked too.
BEZEL_TEST(pure_body_walk_8134,
           case_statement_guard_stdlib_impure_call_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(x: int): int\n"
        "    var r := 0;\n"
        "    case x of\n"
        "      0 when Console.ReadLine()!.Length > 0 => do r := 1; end,\n"
        "      _ => do r := 2; end,\n"
        "    end;\n"
        "    return r;\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kStdlibImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 6u);
}

// The NESTED shape: a `!` inside a `case` arm. Both skips had to be closed
// for this one to be caught — the arm is not walked at all without item 2,
// and the `!` operand is not walked without item 1.
BEZEL_TEST(pure_body_walk_8134,
           case_expression_arm_containing_postfix_bang_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(x: int): int\n"
        "    return (case x of 0 => Console.ReadLine()!.Length, _ => 0);\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kStdlibImpureCall);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) BEZEL_EXPECT_TRUE(d->location.line == 4u);
}

// The ADMIT control: the same arm shape with a `pure`-declared callee.
BEZEL_TEST(pure_body_walk_8134,
           case_expression_arm_pure_call_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class Helper\n"
        "  public static pure method Feed(): string\n"
        "    return \"x\";\n"
        "  end Feed;\n"
        "end Helper;\n"
        "class C\n"
        "  public static pure method F(x: int): string\n"
        "    return (case x of 0 => Helper.Feed(), _ => \"\");\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The ADMIT control for the STANDARD-LIBRARY shape: a member the library
// DOES mark `pure` (stdlib §2.2 `Math.Abs`) is admitted in the arm.
BEZEL_TEST(pure_body_walk_8134,
           case_expression_arm_pure_stdlib_call_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(x: int): int\n"
        "    return (case x of 0 => Math.Abs(-1), _ => 0);\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of item 2: the real driver refuses the program.
BEZEL_TEST(pure_body_walk_8134,
           case_expression_arm_impure_call_rejected_by_driver_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class Helper\n"
        "  public static method Feed(): string\n"
        "    return \"x\";\n"
        "  end Feed;\n"
        "end Helper;\n"
        "class C\n"
        "  public static pure method F(x: int): string\n"
        "    return (case x of 0 => Helper.Feed(), _ => \"\");\n"
        "  end F;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(C.F(0));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kUserImpureCall));
}

// =========================================================================
// ITEM 3 — the throw carve-out checks WHICH constructor runs
// (§22.15's carve-out / §31.4).
// =========================================================================

// §31.4: "The `(message, cause)` chaining constructor is not pure (its
// snapshot builds a fresh `Array`)." §22.15's carve-out admits `new E(...)`
// "only when **(a)** the invoked constructor of `E` is `pure`-declared", so
// the built-in `(message, cause)` construction is NOT admitted at the throw
// operand of a pure body.
BEZEL_TEST(pure_body_walk_8134,
           throw_builtin_exception_cause_constructor_rejected_spec_31_4) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(cause: Exception): string\n"
        "    throw new Exception(\"boom\", cause);\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kCarveOut);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->location.line == 4u);
        // The diagnostic names §31.4's rule — the constructor's own marker.
        BEZEL_EXPECT_TRUE(d->message.find("\xC2\xA7""31.4") !=
                           std::string::npos);
    }
}

// The USER-CLASS twin: a class with a `pure` one-argument constructor AND
// an impure two-argument one. The carve-out used to admit the construction
// because the class had a `pure` constructor SOMEWHERE; the invoked
// constructor is the impure one, so it is rejected.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_impure_constructor_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string) base(message); end "
        "constructor;\n"
        "  public constructor(message: string, cause: Exception) base(message, "
        "cause); end constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(cause: Exception): string\n"
        "    throw new MyError(\"boom\", cause);\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kCarveOut);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->location.line == 8u);
        // The diagnostic names the INVOKED constructor's owner.
        BEZEL_EXPECT_TRUE(d->message.find("MyError") != std::string::npos);
    }
}

// The ADMIT control for the built-in `(message)` form — §31.4 marks it
// `pure`, so the carve-out admits it.
BEZEL_TEST(pure_body_walk_8134,
           throw_builtin_exception_message_constructor_admitted_spec_31_4) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new Exception(\"boom\");\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The ADMIT control for the built-in `()` form — §31.4 marks it `pure` too.
BEZEL_TEST(pure_body_walk_8134,
           throw_builtin_exception_empty_constructor_admitted_spec_31_4) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new Exception();\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The ADMIT control for §31.4's OTHER pure two-argument form:
// `ArgumentException`'s `(message, paramName)`. This is the control that
// shows the two-argument position is not rejected wholesale — the CONSTRUCTOR
// decides, not the arity.
BEZEL_TEST(pure_body_walk_8134,
           throw_argument_exception_param_name_constructor_admitted_spec_31_4) {
    auto r = pureCheck(
        "module M;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new ArgumentException(\"boom\", \"param\");\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The ADMIT control for the USER class: the `pure` one-argument constructor
// is the one invoked, so the carve-out admits it.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_pure_constructor_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string) base(message); end "
        "constructor;\n"
        "  public constructor(message: string, cause: Exception) base(message, "
        "cause); end constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new MyError(\"boom\");\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of item 3: the real driver refuses the program.
BEZEL_TEST(pure_body_walk_8134,
           throw_builtin_exception_cause_constructor_rejected_by_driver_spec_31_4) {
    BEZEL_EXPECT_TRUE(aotRejectsWith(
        "module M;\n"
        "class C\n"
        "  public static pure method F(cause: Exception): string\n"
        "    throw new Exception(\"boom\", cause);\n"
        "  end F;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(C.F(new Exception(\"inner\")));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kCarveOut));
}

// The AOT ADMIT arm of item 3: the carve-out's ADMITTED form compiles,
// LINKS and RUNS — the throw is caught, and the program exits 0.
BEZEL_TEST(pure_body_walk_8134,
           throw_builtin_exception_message_constructor_runs_spec_31_4) {
    BEZEL_EXPECT_TRUE(aotRunsWithExitZero(
        "module M;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new Exception(\"boom\");\n"
        "  end F;\n"
        "end C;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      C.F();\n"
        "    catch var e: Exception => Console.WriteLine(e.Message);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"));
}

// =========================================================================
// FIX-FORWARD #8134 — the INVOKED constructor is identified even when the
// typecheck pass published no selection (§22.15 / §22.5 / §31.4).
// =========================================================================
//
// THE DEFECT THIS CLOSES. §22.15's carve-out admits `new E(...)` "only when
// (a) the INVOKED constructor of `E` is `pure`-declared". Identifying that
// constructor needs the SELECTION the typecheck pass makes — and the pass
// publishes it (`SemaContext::resolvedNewCtor`) in its OVERLOAD arms only.
// Its SINGLE-constructor arms (the local `ctors.size() == 1` tail and the
// imported twin) bind the arguments and check them, but publish nothing.
//
// The carve-out's fallback was therefore an EXACT parameter-count match, and
// §22.5 makes a shorter call legal against a longer parameter list:
//
//   public pure constructor(message: string, code: int := 0) base(message);
//
// invoked as `throw new MyError("neg")` — ONE argument, TWO formals, the
// second filled from its declared default. The parameter count does not
// match, so the fallback found no constructor at all and the carve-out
// refused a program whose invoked constructor IS `pure`-declared — a
// rejection the base did not make.
//
// THE FIX. ONE non-static constructor is the invoked one (a class with a
// single constructor has no other candidate, and the typecheck pass has
// already rejected a call that does not bind to it); otherwise the call's
// arguments are BOUND to each candidate's formals (§22.5), so a defaulted
// formal left unbound — and a named argument — are honoured.

// The STATEMENT form: `throw new MyError("neg")` at the throw operand of a
// pure body, against a `pure` constructor whose second formal declares a
// default the call omits. §22.5 fills that formal from its default, so the
// constructor that runs is the `pure` one and §22.15 (a) admits it.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_pure_constructor_with_default_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new MyError(\"neg\");\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of that row: the program COMPILES, LINKS and RUNS — the throw
// is caught and the program exits 0.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_pure_constructor_with_default_runs_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRunsWithExitZero(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "end MyError;\n"
        "class Program\n"
        "  public static pure method Run(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new MyError(\"neg\");\n"
        "    end;\n"
        "    return x;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(Run(-1));\n"
        "    catch var e: Exception => Console.WriteLine(\"caught \" + e.Message);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"));
}

// The THROW-EXPRESSION form: §22.15's carve-out admits `new E(...)` "only
// as the immediate operand of a `throw` statement or `throw` expression", so
// the expression spelling reaches the same carve-out — and the same
// identification.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_pure_constructor_with_default_throw_expression_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(s: string?): string\n"
        "    return s ?? throw new MyError(\"none\");\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The AOT arm of the throw-expression form.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_pure_constructor_with_default_throw_expression_runs_spec_22_15) {
    BEZEL_EXPECT_TRUE(aotRunsWithExitZero(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "end MyError;\n"
        "class Program\n"
        "  public static pure method Run(s: string?): string\n"
        "    return s ?? throw new MyError(\"none\");\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(Run(\"a\"));\n"
        "    catch var e: Exception => Console.WriteLine(\"caught \" + e.Message);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"));
}

// The DEFAULT-PARAMETER OVERLOAD row (ADMIT): a `pure` constructor with a
// defaulted formal beside an IMPURE sibling the call does not bind (the
// sibling's `cause` is a required formal the call supplies no value for).
// §22.6 leaves exactly one applicable candidate — the `pure` one — so it is
// the constructor that runs, and §22.15 (a) admits the construction.
//
// This is the row that keeps the fix honest in the OVERLOADED case: the
// carve-out may not answer "does E have a `pure` constructor SOMEWHERE" (the
// pre-#8134 question), and it may not answer "the only `pure` constructor E
// has" either — it answers about the constructor the call invokes.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_default_parameter_overload_admitted_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "  public constructor(message: string, cause: Exception) base(message, cause); end "
        "constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new MyError(\"neg\");\n"
        "  end F;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The DEFAULT-PARAMETER OVERLOAD row (REJECT): the same `pure` constructor
// with a defaulted formal, beside an IMPURE sibling of the SAME REQUIRED
// ARITY. §22.6's defaulted-arity tie-break prefers the candidate that fills
// NO default ("adding an optional-parameter overload never re-routes
// existing calls"), so the IMPURE sibling is the constructor the call
// invokes — and §22.15 (a) does not admit it.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_default_parameter_overload_impure_sibling_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "  public constructor(message: string) base(message); end constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new MyError(\"neg\");\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kCarveOut);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->location.line == 8u);
        // The diagnostic names the INVOKED constructor's owner — and, this
        // time, the invoked constructor IS identified (the IMPURE sibling),
        // so the refusal is about its purity, not about its being unknown.
        BEZEL_EXPECT_TRUE(d->message.find("MyError") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("not `pure`-declared") !=
                           std::string::npos);
    }
}

// The SINGLE-IMPURE-CONSTRUCTOR control: one non-static constructor, not
// `pure`-declared, with the same defaulted formal. The carve-out identifies
// it (there is no other candidate) and refuses it for its impurity — the
// identification is not a blanket admission.
BEZEL_TEST(pure_body_walk_8134,
           throw_user_exception_impure_constructor_with_default_rejected_spec_22_15) {
    auto r = pureCheck(
        "module M;\n"
        "public class MyError: Exception\n"
        "  public constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "end MyError;\n"
        "class C\n"
        "  public static pure method F(): string\n"
        "    throw new MyError(\"neg\");\n"
        "  end F;\n"
        "end C;\n");
    const bezel::Diagnostic* d = theDiagnosticContaining(r, kCarveOut);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->location.line == 7u);
        BEZEL_EXPECT_TRUE(d->message.find("not `pure`-declared") !=
                           std::string::npos);
    }
}

// The IMPORTED twin: the library module exports the class, the importing
// module throws it in a pure body. The imported single-constructor path
// (`ExprTC::checkImportedNewExprArgs`) publishes no selection either, so the
// carve-out has to identify the invoked constructor ACROSS the module
// boundary — where the constructor's purity is the artifact's own marker
// (`MemberSymbol::isPure`, the both-sources arm of `constructorIsPure`).
//
// The oracle is `runAotModules` — the strict multi-module AOT oracle (the
// real driver, a library artifact built with `compileObject`, clang-link,
// native exec). The program must COMPILE, LINK and RUN: the throw is caught
// in the importer and the program exits 0.
BEZEL_TEST(pure_body_walk_8134,
           throw_imported_exception_pure_constructor_with_default_runs_spec_22_15) {
    const std::string lib =
        "module Lib;\n"
        "public class MyError: Exception\n"
        "  public pure constructor(message: string, code: int := 0) base(message); end "
        "constructor;\n"
        "end MyError;\n";
    const std::string app =
        "module App;\n"
        "import Lib;\n"
        "class Program\n"
        "  public static pure method Run(x: int): int\n"
        "    if x < 0 then\n"
        "      throw new MyError(\"neg\");\n"
        "    end;\n"
        "    return x;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(Run(-1));\n"
        "    catch var e: Exception => Console.WriteLine(\"caught \" + e.Message);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto r = bezel::test::runAotModules(
        {bezel::test::ModuleSource{"Lib", lib},
         bezel::test::ModuleSource{"App", app}},
        "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}
