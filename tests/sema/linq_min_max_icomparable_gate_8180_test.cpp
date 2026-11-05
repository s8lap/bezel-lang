// fix #8180 (stdlib §5.1, spec §14.2 *Interface conformance*, §26.5.1, v0.355):
// a no-selector `Min()` / `Max()` over an element that PROVABLY fails
// `T: IComparable<T>` is refused with a located diagnostic (it emitted
// malformed IR), and the same `where T: IComparable<T>` clause is an
// APPLICABILITY filter against a user extension of the same name.
//
// SPEC BASIS (QUOTED, v0.355).
//   stdlib §5.1: "`Min` and `Max` without a selector are generic over
//     comparable types: `public extension IEnumerable<T> where T:
//     IComparable<T>` / `public method Min(): T;` / `public method Max(): T;`"
//   spec §14.2 *Interface conformance*: "For an interface I and a candidate
//     type T, T **conforms to** I exactly when one of the following holds" —
//     rules 1-7 name classes, records, interfaces, enums, primitives (through
//     their stdlib declarations), type parameters and choices; no rule names a
//     FUNCTION type.
//   spec §14.2: "A type-bound constraint — naming a class or interface —
//     implies non-null T" (so `int?` does not satisfy `T: IComparable<T>`).
//   spec §3.5: "the stdlib `Boolean` wrapper deliberately declares no
//     `IComparable`".
//   spec §26.5.1: "specificity ranks the applicable candidates: extension
//     members of the called name that match the call's arity and argument
//     types, from blocks whose head matches R ..., with block-level `where`
//     clauses satisfied by the arguments inferred from the receiver. A `where`
//     clause is an *applicability* filter, never a ranking signal."
//
// MEASURED ON THE PARENT (r30-c1 @ 737e6d0a):
//   * `Array<(int) -> int>.Min()` / `.Max()`, with or without
//     `extension Object { Min(): int }`: "Both operands to ICmp instruction are
//     not of the same type! %28 = icmp slt ptr %25, i32 %27" — `bezelc`
//     aborted, "IR verification failed (malformed IR emitted)".
//   * the same over a lazy receiver (`a.Where(...).Min()`), an
//     `IEnumerable<(int) -> int>` parameter, `Array<bool>` and `Array<int?>`:
//     the same malformed-IR abort.
//   * `Array<K>` (K a class with no `IComparable`) / `Array<P>` (P a record
//     with no `IComparable`) with `extension Object { Min(): int => 4242 }`:
//     REFUSED with the §5.1 diagnostic although the extension is the only
//     applicable candidate.
//
// THE ORACLE. `runAot` (the strict AOT path). Reject rows assert the SPECIFIC
// diagnostic and that it is not the IR-verifier abort; run rows assert the exit
// code and the EXACT stdout. Leak-checked rows of the same shapes live in
// tests/codegen/linq_icomparable_applicability_8180_test.cpp (runAotChecked).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string mainOnly(const std::string& prelude, const std::string& body) {
    return "module M;\n" + prelude +
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The refusal the stdlib §5.1 gate reports, NOT the IR-verifier abort.
void expectNotComparableRefusal(const JitResult& r, const char* what) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "without a selector requires the element type to implement "
           "IComparable<T>"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, what));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

const char* kObjectMin =
    "extension Object\n"
    "  public method Min(): int => 4242;\n"
    "  public method Max(): int => 4343;\n"
    "end;\n";

}  // namespace

// ---------------------------------------------------------------------------
// R1 — the issue's probe n4c: `Array<(int) -> int>.Min()`, no extension.
// A function type conforms to no interface (§14.2 rules 1-7), so the §5.1
// overload does not apply and the call is refused.
// PARENT: malformed IR (`icmp slt ptr, i32`), `bezelc` aborted.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           function_element_min_is_refused_not_malformed_ir) {
    JitResult r = runAot(mainOnly("",
        "    var a := new Array<(int) -> int>();\n"
        "    a.Add(lambda(x) => x);\n"
        "    Console.WriteLine(a.Min());\n"));
    expectNotComparableRefusal(r, "a function type does not implement");
}

BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           function_element_max_is_refused_not_malformed_ir) {
    JitResult r = runAot(mainOnly("",
        "    var a := new Array<(int) -> int>();\n"
        "    a.Add(lambda(x) => x);\n"
        "    Console.WriteLine(a.Max());\n"));
    expectNotComparableRefusal(r, "a function type does not implement");
}

// The lazy-receiver and `IEnumerable<T>`-parameter twins of R1.
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           function_element_min_on_a_lazy_receiver_is_refused) {
    JitResult r = runAot(mainOnly("",
        "    var a := new Array<(int) -> int>();\n"
        "    a.Add(lambda(x) => x);\n"
        "    Console.WriteLine(a.Where(lambda(f) => true).Min());\n"));
    expectNotComparableRefusal(r, "a function type does not implement");
}

BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           function_element_min_on_an_ienumerable_parameter_is_refused) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  static method F(xs: IEnumerable<(int) -> int>): int\n"
        "    var m := xs.Min();\n"
        "    return 1;\n"
        "  end F;\n"
        "  public static method Main(): int\n"
        "    var a := new Array<(int) -> int>();\n"
        "    a.Add(lambda(x) => x);\n"
        "    Console.WriteLine(F(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectNotComparableRefusal(r, "a function type does not implement");
}

// `bool` (spec §3.5: `Boolean` "declares no `IComparable`") and a nullable
// primitive (§14.2: a type-bound constraint "implies non-null T").
// PARENT: both aborted with malformed IR.
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           bool_element_min_is_refused_not_malformed_ir) {
    JitResult r = runAot(mainOnly("",
        "    var a := new Array<bool>();\n"
        "    a.Add(true);\n"
        "    a.Add(false);\n"
        "    Console.WriteLine(a.Min());\n"));
    expectNotComparableRefusal(r, "`bool` does not implement");
}

BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           nullable_int_element_min_is_refused_not_malformed_ir) {
    JitResult r = runAot(mainOnly("",
        "    var a := new Array<int?>();\n"
        "    a.Add(4);\n"
        "    a.Add(2);\n"
        "    Console.WriteLine(a.Min());\n"));
    expectNotComparableRefusal(r, "`int?` does not implement");
}

// ---------------------------------------------------------------------------
// R2 — the issue's probe n4: the same function element WITH
// `extension Object { Min() }`. The §5 block's `where T: IComparable<T>` is
// unsatisfied, so (§26.5.1) the §5 overload is not an applicable candidate and
// the user's extension — the only applicable one — answers.
// PARENT: malformed IR.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           function_element_min_answers_the_applicable_extension) {
    JitResult r = runAot(mainOnly(kObjectMin,
        "    var a := new Array<(int) -> int>();\n"
        "    a.Add(lambda(x) => x);\n"
        "    Console.WriteLine(a.Min());\n"
        "    Console.WriteLine(a.Max());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n4343\n");
}

// The issue's class-element control (probe ic1): a class `K` that is not
// `IComparable`. PARENT: REFUSED with "K does not implement IComparable<T>"
// although the extension applies.
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           non_comparable_class_element_min_answers_the_applicable_extension) {
    JitResult r = runAot(mainOnly(
        std::string("class K\n"
                    "  public X: int;\n"
                    "  public constructor(x: int)\n"
                    "    X := x;\n"
                    "  end;\n"
                    "end K;\n") + kObjectMin,
        "    var a := new Array<K>();\n"
        "    var k := new K(1);\n"
        "    a.Add(k);\n"
        "    Console.WriteLine(a.Min());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// A record that is not `IComparable<Self>` (the #8092 record verdict).
// PARENT: REFUSED with "P does not implement IComparable<T>".
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           non_comparable_record_element_min_answers_the_applicable_extension) {
    JitResult r = runAot(mainOnly(
        std::string("record P(X: int);\n") + kObjectMin,
        "    var a := new Array<P>();\n"
        "    a.Add(new P(3));\n"
        "    Console.WriteLine(a.Min());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// `bool` / `int?` elements with the extension. PARENT: malformed IR.
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           bool_and_nullable_elements_min_answer_the_applicable_extension) {
    JitResult r = runAot(mainOnly(kObjectMin,
        "    var a := new Array<bool>();\n"
        "    a.Add(true);\n"
        "    Console.WriteLine(a.Min());\n"
        "    var b := new Array<int?>();\n"
        "    b.Add(4);\n"
        "    Console.WriteLine(b.Max());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n4343\n");
}

// ---------------------------------------------------------------------------
// CONTROLS (silent-acceptance mirror) — a LEGAL `Min()` / `Max()` keeps the §5
// answer with the same `extension Object` in scope: the §5 block applies and
// its `IEnumerable<T>` head is more specific than `Object` (§26.5.1). Each
// prints the §5 result, NOT 4242 / 4343. PARENT: the same output.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           int_and_string_elements_keep_the_builtin_min_max) {
    JitResult r = runAot(mainOnly(kObjectMin,
        "    var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    a.Add(3);\n"
        "    a.Add(9);\n"
        "    Console.WriteLine(a.Min());\n"
        "    Console.WriteLine(a.Max());\n"
        "    var s := new Array<string>();\n"
        "    s.Add(\"pear\");\n"
        "    s.Add(\"apple\");\n"
        "    s.Add(\"zoo\");\n"
        "    Console.WriteLine(s.Min());\n"
        "    Console.WriteLine(s.Max());\n"
        "    var d := new Array<double>();\n"
        "    d.Add(2.5);\n"
        "    d.Add(1.5);\n"
        "    Console.WriteLine(d.Min());\n"
        "    var c := new Array<char>();\n"
        "    c.Add('q');\n"
        "    c.Add('b');\n"
        "    Console.WriteLine(c.Max());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n9\napple\nzoo\n1.5\nq\n");
}

BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           comparable_class_and_record_elements_keep_the_builtin_min) {
    JitResult r = runAot(mainOnly(
        std::string("class K: IComparable<K>\n"
                    "  public X: int;\n"
                    "  public constructor(x: int)\n"
                    "    X := x;\n"
                    "  end;\n"
                    "  public method CompareTo(other: K): int => X - other.X;\n"
                    "end K;\n"
                    "record P(X: int): IComparable<P>\n"
                    "  public method CompareTo(other: P): int => X - other.X;\n"
                    "end P;\n") + kObjectMin,
        "    var a := new Array<K>();\n"
        "    var k7 := new K(7);\n"
        "    var k2 := new K(2);\n"
        "    a.Add(k7);\n"
        "    a.Add(k2);\n"
        "    Console.WriteLine(a.Min().X);\n"
        "    var p := new Array<P>();\n"
        "    p.Add(new P(3));\n"
        "    p.Add(new P(1));\n"
        "    Console.WriteLine(p.Max().X);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n3\n");
}

// No extension: the legal no-selector forms over the eager and the lazy
// receivers are unchanged.
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           legal_min_max_without_an_extension_are_unchanged) {
    JitResult r = runAot(mainOnly("",
        "    var a := new Array<int>();\n"
        "    a.Add(4);\n"
        "    a.Add(9);\n"
        "    a.Add(2);\n"
        "    Console.WriteLine(a.Min());\n"
        "    Console.WriteLine(a.Where(lambda(x) => x > 3).Min());\n"
        "    Console.WriteLine(a.Select(lambda(x) => x * 2).Max());\n"
        "    var s := new Array<string>();\n"
        "    s.Add(\"pear\");\n"
        "    s.Add(\"apple\");\n"
        "    Console.WriteLine(s.Min());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n4\n18\napple\n");
}

// The selector form over a function element is NOT gated (stdlib §5:
// `Min<U>(selector: (T) -> U): U` carries no constraint on T).
BEZEL_TEST(sema_linq_min_max_icomparable_gate_8180,
           selector_min_over_function_elements_still_compiles) {
    JitResult r = runAot(mainOnly("",
        "    var a := new Array<(int) -> int>();\n"
        "    a.Add(lambda(x) => x + 1);\n"
        "    a.Add(lambda(x) => x * 10);\n"
        "    Console.WriteLine(a.Min(lambda(f) => f(3)));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}
