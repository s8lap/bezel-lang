// Regression tests for #6318: `case this of …` inside a lambda formed in a
// choice instance method emitted malformed IR ("Basic Block in function
// 'lam$0' does not have terminator! label %case.end"), so the program was
// rejected by the IR verifier. Spec: §35.2 (`this` in a choice instance method
// is a borrow of the receiver value), §27.3 (lambda expressions), and §35.4
// (value-like choices, whose payloads hold no borrows).
//
// Cause: Emitter::choiceTypeOf recovers the choice type of `this` by
// reverse-mapping the function being emitted to its method. A lambda body is
// emitted into its own function, which none of those recoveries knows, so the
// scrutinee of `case this` carried no choice type. patternMatches' choice
// branch then never fired and the arm chain produced no terminator. The fix
// consults lambdaFnEnclosingType_ (the lambda's lexically enclosing type) for a
// non-generic choice, as the #6320 arm of currentReceiverRecordType does for a
// record.
//
// Most accept rows keep the lambda inside the method: bound to a local and
// called, or passed to a parameter and called there. Escapes follow §9.4
// (v0.340): "A closure formed in a copyable value kind's member body captures
// the receiver value by copy into its heap capture cell", contributing what a
// captured local of that type would.
// - A choice that holds borrows: the copy carries them, so §28.3's sink rule
//   with §9.7's closure bullet (§35.4) rejects the escape. That is #6630, and
//   #6318's own repro is that shape (it stores the closure via `a.Add(...)`
//   over a `Widget` payload), so it is the REJECTION row below.
// - A choice that holds NO borrow: the escape is admitted, and the closure
//   reads its own copy (#6721 makes the copy). #6634's p5 (`Tag.Maker`
//   returned, then a Clobber call over the dead frame) is the escape row
//   below.
// Generic choices (#6638) and async lambdas (#6640) are separate issues.
//
// The accept rows run under runAotChecked: checked disposal plus a leak check
// over the native run.
//
// per §35.2, §27.3, §28.3, §9.4 (v0.340)

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// The program's captured stdout, or the sentinel "<did-not-run>" when the
// program was rejected, crashed, timed out, exited non-zero or leaked. A reject
// never matches an expectation, so a row cannot pass on an empty string.
std::string stdoutOf(const std::string& program) {
    auto r = runAotChecked(program);
    return (r.ran && r.exitCode == 0) ? r.stdoutText
                                      : std::string("<did-not-run>");
}
}  // namespace

// §27.3: an expression-bodied lambda formed in a choice method; the `case this`
// payload arm reads the active case's payload.
BEZEL_TEST(choice_case_this_lambda_6318, expression_lambda_case_this_payload_arm) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Describe(): int "
        "    var f := lambda(): int => (case this of A(n) => n * 10 + 1, B => -1); "
        "    return f(); "
        "  end Describe; "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.A(7).Describe()); "
        "  end Main; "
        "end Program;"), "71\n");
}

// The same lambda with the payload-free case active: the bare `B` arm.
BEZEL_TEST(choice_case_this_lambda_6318, expression_lambda_case_this_payload_free_arm) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Describe(): int "
        "    var f := lambda(): int => (case this of A(n) => n * 10 + 1, B => -1); "
        "    return f(); "
        "  end Describe; "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.B.Describe()); "
        "  end Main; "
        "end Program;"), "-1\n");
}

// §27.3: a block-bodied lambda whose body is a statement-form `case this`
// (lowerCaseStmt reads the same scrutinee type through choiceTypeOf).
BEZEL_TEST(choice_case_this_lambda_6318, block_lambda_statement_case_this) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Describe(): int "
        "    var f := lambda(): int => do "
        "      var r := 0; "
        "      case this of "
        "        A(n) => r := n * 10 + 1, "
        "        B => r := -1 "
        "      end case; "
        "      return r; "
        "    end; "
        "    return f(); "
        "  end Describe; "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.A(7).Describe()); "
        "    Console.WriteLine(Tag.B.Describe()); "
        "  end Main; "
        "end Program;"), "71\n-1\n");
}

// §27.3: a lambda nested in a lambda; the inner one carries the `case this`.
BEZEL_TEST(choice_case_this_lambda_6318, nested_lambda_case_this) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Describe(): int "
        "    var f := lambda(): int => (lambda(): int => (case this of A(n) => n * 10 + 1, B => -1))(); "
        "    return f(); "
        "  end Describe; "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.A(7).Describe()); "
        "    Console.WriteLine(Tag.B.Describe()); "
        "  end Main; "
        "end Program;"), "71\n-1\n");
}

// §35.4 borrow-bearing choice (a class payload holds a borrow), used in a
// lambda that does not escape the method: the captured borrow stays within the
// call, so the program is valid under §9.7.
BEZEL_TEST(choice_case_this_lambda_6318, non_escaping_class_payload_case_this) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class Widget "
        "  private _s: string; "
        "  public constructor(s: string) "
        "    _s := s; "
        "  end constructor; "
        "  public method GetLabel(): string => _s; "
        "end Widget; "
        "public choice Held "
        "  One(w: Widget), "
        "  Nothing; "
        "  public method Show(): string "
        "    var f := lambda(): string => (case this of One(x) => x.GetLabel(), Nothing => \"none\"); "
        "    return f(); "
        "  end Show; "
        "end Held; "
        "public class Program "
        "  public static method Main() "
        "    using var w := new Widget(\"hi\"); "
        "    var h := Held.One(w); "
        "    Console.WriteLine(h.Show()); "
        "    Console.WriteLine(Held.Nothing.Show()); "
        "  end Main; "
        "end Program;"), "hi\nnone\n");
}

// A choice with payload-free cases only (value-like): the lambda's `case this`
// selects among bare cases.
BEZEL_TEST(choice_case_this_lambda_6318, payload_free_only_choice_case_this) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Color "
        "  Red, "
        "  Green; "
        "  public method Code(): int "
        "    var f := lambda(): int => (case this of Red => 1, Green => 2); "
        "    return f(); "
        "  end Code; "
        "end Color; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Color.Green.Code()); "
        "    Console.WriteLine(Color.Red.Code()); "
        "  end Main; "
        "end Program;"), "2\n1\n");
}

// A runtime-managed payload (`string`, classification-neutral per §35.4): the
// lambda's `case this` binds the string payload.
BEZEL_TEST(choice_case_this_lambda_6318, string_payload_case_this) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag2 "
        "  S(s: string), "
        "  E; "
        "  public method Name(): string "
        "    var f := lambda(): string => (case this of S(s) => s, E => \"e\"); "
        "    return f(); "
        "  end Name; "
        "end Tag2; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag2.S(\"x\").Name()); "
        "    Console.WriteLine(Tag2.E.Name()); "
        "  end Main; "
        "end Program;"), "x\ne\n");
}

// The lambda is passed to a method and invoked there, never stored.
BEZEL_TEST(choice_case_this_lambda_6318, lambda_passed_to_method_not_stored) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Via(f: () -> int): int => f(); "
        "  public method Run(): int => this.Via(lambda(): int => (case this of A(n) => n * 10 + 1, B => -1)); "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.A(7).Run()); "
        "    Console.WriteLine(Tag.B.Run()); "
        "  end Main; "
        "end Program;"), "71\n-1\n");
}

// The declaration order is not significant: the choice follows the class that
// uses it.
BEZEL_TEST(choice_case_this_lambda_6318, choice_declared_after_use) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.A(7).Describe()); "
        "    Console.WriteLine(Tag.B.Describe()); "
        "  end Main; "
        "end Program; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Describe(): int "
        "    var f := lambda(): int => (case this of A(n) => n * 10 + 1, B => -1); "
        "    return f(); "
        "  end Describe; "
        "end Tag;"), "71\n-1\n");
}

// Controls the fix must not change: a `case this` outside any lambda (both
// statement and expression forms), a `this` method call inside a lambda, and a
// lambda whose `case` scrutinee is a parameter rather than `this`. All three
// pass before the fix as well.
BEZEL_TEST(choice_case_this_lambda_6318, control_case_this_outside_lambda) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Plain(): int "
        "    var r := 0; "
        "    case this of "
        "      A(n) => r := n, "
        "      B => r := -1 "
        "    end case; "
        "    return r; "
        "  end Plain; "
        "  public method Plain2(): int => (case this of A(n) => n, B => -1); "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.A(7).Plain()); "
        "    Console.WriteLine(Tag.B.Plain2()); "
        "  end Main; "
        "end Program;"), "7\n-1\n");
}

BEZEL_TEST(choice_case_this_lambda_6318, control_this_method_call_in_lambda) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Other(): int => 5; "
        "  public method Describe(): int "
        "    var f := lambda(): int => this.Other(); "
        "    return f(); "
        "  end Describe; "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Tag.A(7).Describe()); "
        "    Console.WriteLine(Tag.B.Describe()); "
        "  end Main; "
        "end Program;"), "5\n5\n");
}

BEZEL_TEST(choice_case_this_lambda_6318, control_case_on_parameter_in_lambda) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Pick(o: Tag): int "
        "    var f := lambda(): int => (case o of A(n) => n, B => -1); "
        "    return f(); "
        "  end Pick; "
        "end Tag; "
        "public class Program "
        "  public static method Main() "
        "    var t := Tag.B; "
        "    Console.WriteLine(t.Pick(Tag.A(7))); "
        "    Console.WriteLine(t.Pick(Tag.B)); "
        "  end Main; "
        "end Program;"), "7\n-1\n");
}

// #6634 p5, the literal repro: a closure over a borrow-free choice `this`,
// returned from `Maker`, outlives `MakeFrom`'s frame. §9.4 (v0.340) admits it
// by copy, so it prints `A7`. Clobber reuses the dead frame's stack, so a
// closure that read the receiver through its stack address would print
// garbage or fail the match. Before #6721 that was MatchFailureException (exit
// 70), and stack-use-after-return under ASan.
// per §9.4 (v0.340), §35.2, §28.3
BEZEL_TEST(choice_case_this_lambda_6318, escape_by_return_reads_copy_spec_9_4_6634_p5) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; "
        "public choice Tag "
        "  A(n: int), "
        "  B; "
        "  public method Maker(): () -> string "
        "    return lambda(): string => (case this of A(n) => $\"A{n}\", B => \"B\"); "
        "  end Maker; "
        "end Tag; "
        "public class Program "
        "  private static method Clobber(a: int, b: int, c: int, d: int): int "
        "    var p1 := a * 3 + b; "
        "    var p2 := c * 5 + d; "
        "    var p3 := p1 + p2; "
        "    return p3 + a + b + c + d; "
        "  end Clobber; "
        "  private static method MakeFrom(): () -> string "
        "    var t := Tag.A(7); "
        "    return t.Maker(); "
        "  end MakeFrom; "
        "  public static method Main() "
        "    var f := MakeFrom(); "
        "    Console.WriteLine(Clobber(111, 222, 333, 444)); "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;"),
        "3774\nA7\n");
}

// #6318's literal shape: the `case this` lambda over a class-payload choice is
// stored into the caller's collection from `Hook`. That is the escape the
// sink rule forbids, so the program is now rejected at the store, with the
// sink-rule diagnostic, instead of reaching codegen. MEASURED on 25e44bdc
// (before #6630): "IR verification failed". With #6630 and without this
// codegen fix, the ownership check already rejects it, so this row pins
// that the shape stays unreachable, not the codegen fix itself.
// per §28.3 (v0.340) sink rule ("in any collection"), §9.7, §35.4
BEZEL_TEST(choice_case_this_lambda_6318, escape_into_collection_rejected_sink_rule) {
    auto r = runAot(
        "module M; "
        "public class Widget "
        "  private _s: string; "
        "  public constructor(s: string) "
        "    _s := s; "
        "  end constructor; "
        "  public method GetLabel(): string => _s; "
        "end Widget; "
        "public choice Held "
        "  One(w: Widget), "
        "  Nothing; "
        "  public method Hook(a: Array<() -> string>) "
        "    a.Add(lambda(): string => (case this of One(x) => x.GetLabel(), Nothing => \"none\")); "
        "  end Hook; "
        "end Held; "
        "public class Program "
        "  public static method Main() "
        "    var a := new Array<() -> string>(); "
        "    using var w := new Widget(\"hi\"); "
        "    var h := Held.One(w); "
        "    h.Hook(a); "
        "    Console.WriteLine(a[0]()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool sink = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("closure stored in a collection (`.Add(...)`) "
                           "captures `this`") != std::string::npos &&
            d.message.find("(§28.3 sink rule, v0.340") != std::string::npos)
            sink = true;
    BEZEL_EXPECT_TRUE(sink);
    // The rejection is the ownership check, never the codegen verifier.
    bool irFail = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("IR verification failed") != std::string::npos)
            irFail = true;
    BEZEL_EXPECT_FALSE(irFail);
}
