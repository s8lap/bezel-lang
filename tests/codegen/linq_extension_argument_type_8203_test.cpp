// fix #8203 (spec §26.5.1, v0.355): the leak-checked (runAotChecked:
// generated-code ASan + LeakSanitizer) rows for a §5 LINQ operator whose
// argument is NOT a function value, with a same-named user `extension Object`
// member in scope.
//
// THE DEFECT. §26.5.1 (v0.355) ranks "the APPLICABLE candidates": extension
// members "that match the call's arity and ARGUMENT TYPES, from blocks whose
// head matches R ..., with block-level `where` clauses satisfied by the
// arguments inferred from the receiver. A `where` clause is an *applicability*
// filter, never a ranking signal."
//
// The §5 block's applicability filter (fix #7967) reads the call's argument
// types, but the arguments are visited AFTER the callee's member access is
// resolved — so at the ranking site the argument's type is not yet known. The
// filter could classify a LITERAL and a typed BINDING, and reported UNKNOWN
// for everything else — a CALL RESULT and a CONCATENATION among them. The §5
// `Where` was then admitted by name and, its `IEnumerable<T>` head being a
// subtype of `Object` (§26.5.1), it BEAT the user's applicable
// `extension Object { Where(s: string) }`: the program COMPILED and printed
// GARBAGE (the §5 operator ran with a `string` where its `(T) -> bool`
// predicate belongs) and leaked the lazy pipeline.
//
// The fix types the ONE argument of a §5 element-lambda operator BEFORE the
// callee's member access is resolved, so the filter reads a real type.
//
// MEASURED ON THE PARENT (r30-c1 @ b52409a7), the issue's own repros:
//
//   a.Where(Str())          COMPILES; PRINTS binary garbage (a pointer
//                            printed as an int), rc 0.
//   a.Where("x" + "y")      COMPILES; PRINTS the same garbage, rc 0.
//   a.Where("xy")           COMPILES; PRINTS 4242 — the user's extension
//                            already answered (the #7967 fix).
//
// The rows are LOOPED (100 iterations) so a per-call leak cannot hide.
//
// SPEC BASIS. §26.5.1 *Specificity ordering* (v0.355), §26.5 rules 2-3
// (v0.355), stdlib §5.1 (the §5 `Where` overload's `(T) -> bool` parameter),
// §27.3 (a lambda is a function value) and §2.3 (the auto-imported stdlib §5
// extension module).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's REPRO, verbatim in shape: a user `extension Object { Where(s:
// string) }` and a `a.Where(<call result>)` over an `Array<int>`.
std::string whereCallResult() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  static method Str(): string => \"q\";\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    Console.WriteLine(a.Where(Str()));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The issue's second spelling: the same extension and a CONCATENATED string
// argument.
std::string whereConcatenation() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    Console.WriteLine(a.Where(\"x\" + \"y\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 1 — a string LITERAL argument already answered the user's
// extension (the #7967 fix) — 4242.
std::string whereLiteral() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    Console.WriteLine(a.Where(\"xy\"));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 2 — a LAMBDA argument keeps the §5 `Where`: the §5 overload IS
// applicable there (its parameter is the `(T) -> bool` the lambda forms), so
// it stays the more specific candidate and answers — 2 of the 3 elements
// survive the filter.
std::string whereLambda() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1); a.Add(2); a.Add(3);\n"
        "    Console.WriteLine(a.Where(lambda(x: int): bool => x > 1)"
        ".Count());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// CONTROL 3 — a METHOD REFERENCE argument keeps the §5 `Where` too (the
// reference's type is the function type the §5 parameter declares) — 1 of the
// 3 elements is even.
std::string whereMethodReference() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Even(x: int): bool => x mod 2 = 0;\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1); a.Add(2); a.Add(3);\n"
        "    Console.WriteLine(a.Where(Even).Count());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The LOOPED shape: 100 iterations of the issue's two spellings plus the
// literal control, so a per-call leak cannot hide.
std::string looped() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Where(s: string): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  static method Str(): string => \"q\";\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    var t := 0;\n"
        "    while i < 100 do\n"
        "      var a := new Array<int>();\n"
        "      a.Add(1);\n"
        "      t := t + a.Where(Str()) + a.Where(\"x\" + \"y\") +"
        " a.Where(\"xy\");\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// R1 — the issue's repro: `a.Where(Str())`. A `string` argument does not match
// the §5 `Where` overload's `(T) -> bool` parameter, so the §5 overload is NOT
// applicable and the user's `extension Object { Where(s: string) }` — the only
// APPLICABLE candidate — answers 4242.
//
// PARENT (r30-c1 @ b52409a7): the program COMPILES and PRINTS binary
// garbage (the §5 operator ran with a `string` where its predicate belongs)
// and leaks the lazy pipeline.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_extension_argument_type_8203,
           where_with_a_call_result_argument_answers_the_applicable_extension) {
    JitResult r = runAotChecked(whereCallResult());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R2 — the issue's second spelling: `a.Where("x" + "y")`. Same verdict — 4242.
//
// PARENT: the same garbage.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_extension_argument_type_8203,
           where_with_a_concatenated_argument_answers_the_applicable_extension) {
    JitResult r = runAotChecked(whereConcatenation());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R3 — the LOOPED row: 100 x (4242 + 4242 + 4242) = 1272600.
//
// PARENT: the program PRINTS GARBAGE (and leaks) — the row fails on the
// stdout assertion.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_extension_argument_type_8203,
           where_with_a_non_function_argument_is_leak_free_over_a_hundred_calls) {
    JitResult r = runAotChecked(looped());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1272600\n");
}

// ---------------------------------------------------------------------------
// C1 — CONTROL: a string LITERAL argument already answered the user's
// extension — 4242.
//
// PARENT: also 4242 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_extension_argument_type_8203,
           where_with_a_literal_argument_keeps_the_extension_answer) {
    JitResult r = runAotChecked(whereLiteral());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// C2 — CONTROL: a LAMBDA argument keeps the §5 `Where` (it IS applicable
// there) — 2.
//
// PARENT: also 2 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_extension_argument_type_8203,
           where_with_a_lambda_argument_keeps_the_builtin) {
    JitResult r = runAotChecked(whereLambda());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ---------------------------------------------------------------------------
// C3 — CONTROL: a METHOD REFERENCE argument keeps the §5 `Where` — 1.
//
// PARENT: also 1 — this row must NOT change.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_linq_extension_argument_type_8203,
           where_with_a_method_reference_argument_keeps_the_builtin) {
    JitResult r = runAotChecked(whereMethodReference());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}
