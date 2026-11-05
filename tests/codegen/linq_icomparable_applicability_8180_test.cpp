// fix #8180 (stdlib §5.1, spec §14.2 *Interface conformance*, §26.5.1, v0.355):
// the leak-checked (runAotChecked: generated-code ASan + LeakSanitizer) rows for
// the `where T: IComparable<T>` applicability filter. A no-selector `Min()` /
// `Max()` over an element that provably fails `T: IComparable<T>` is not an
// applicable §5 candidate, so a same-named `extension Object` member answers;
// over a comparable element the §5 overload keeps the call.
//
// QUOTED (v0.355): stdlib §5.1 "`Min` and `Max` without a selector are generic
// over comparable types: `public extension IEnumerable<T> where T:
// IComparable<T>`"; spec §26.5.1 "A `where` clause is an *applicability*
// filter, never a ranking signal."
//
// MEASURED ON THE PARENT (r30-c1 @ 737e6d0a): every function / bool / `int?`
// row below aborted in `verifyModule` ("Both operands to ICmp instruction are
// not of the same type!"); the class and record rows were REFUSED with the
// §5.1 diagnostic. The control rows printed the same output.
//
// The rows are LOOPED (100 iterations) so a per-call leak cannot hide. The
// function-element rows use an EMPTY array: storing a lambda in an `Array` leaks
// 64 B on the parent independently of this fix (it reproduces with no `Min` at
// all), and is out of this fix's scope.
//
// The diagnostic (reject) rows live in
// tests/sema/linq_min_max_icomparable_gate_8180_test.cpp.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kObjectMinMax =
    "extension Object\n"
    "  public method Min(): int => 4242;\n"
    "  public method Max(): int => 4343;\n"
    "end;\n";

// `module M;` + `prelude` + a `Main` running `loopBody` 100 times (it adds to
// `t`) and printing `t`, then `tail`.
std::string looped(const std::string& prelude, const std::string& loopBody,
                   const std::string& tail = "") {
    return "module M;\n" + prelude +
           "class C\n"
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    var t := 0;\n"
           "    while i < 100 do\n" +
           loopBody +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(t);\n" +
           tail +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// The issue's shape: function elements, the extension answers (100 x
// (4242 + 4343) = 858500). PARENT: malformed IR.
BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           function_element_min_max_answer_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(kObjectMinMax,
        "      var a := new Array<(int) -> int>();\n"
        "      t := t + a.Min() + a.Max();\n"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "858500\n");
}

// A class that is not `IComparable`. PARENT: REFUSED.
BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           non_comparable_class_element_min_answers_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(
        std::string("class K\n"
                    "  public X: int;\n"
                    "  public constructor(x: int)\n"
                    "    X := x;\n"
                    "  end;\n"
                    "end K;\n") + kObjectMinMax,
        "      var a := new Array<K>();\n"
        "      var k := new K(i);\n"
        "      a.Add(k);\n"
        "      t := t + a.Min();\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "424200\n");
}

// A non-comparable record, `bool` and `int?` elements. PARENT: the record row
// REFUSED; the `bool` / `int?` rows malformed IR.
BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           record_bool_and_nullable_elements_answer_the_extension_without_leaking) {
    JitResult r = runAotChecked(looped(
        std::string("record P(X: int);\n") + kObjectMinMax,
        "      var p := new Array<P>();\n"
        "      p.Add(new P(i));\n"
        "      var b := new Array<bool>();\n"
        "      b.Add(true);\n"
        "      var n := new Array<int?>();\n"
        "      n.Add(i);\n"
        "      t := t + p.Min() + b.Max() + n.Min();\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 100 x (4242 + 4343 + 4242)
    BEZEL_EXPECT_EQ(r.stdoutText, "1282700\n");
}

// CONTROL (silent-acceptance mirror): int / string / comparable-class elements
// keep the §5 `Min()` / `Max()` with the extension in scope. 100 x (3 + 9) = 1200.
// PARENT: the same output.
BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           comparable_elements_keep_the_builtin_min_max_without_leaking) {
    JitResult r = runAotChecked(looped(
        std::string("class K: IComparable<K>\n"
                    "  public X: int;\n"
                    "  public constructor(x: int)\n"
                    "    X := x;\n"
                    "  end;\n"
                    "  public method CompareTo(other: K): int => X - other.X;\n"
                    "end K;\n") + kObjectMinMax,
        "      var a := new Array<int>();\n"
        "      a.Add(5);\n"
        "      a.Add(3);\n"
        "      a.Add(9);\n"
        "      t := t + a.Min() + a.Max();\n",
        "    var s := new Array<string>();\n"
        "    s.Add(\"pear\");\n"
        "    s.Add(\"apple\");\n"
        "    s.Add(\"zoo\");\n"
        "    Console.WriteLine(s.Min());\n"
        "    Console.WriteLine(s.Max());\n"
        "    var ks := new Array<K>();\n"
        "    var k7 := new K(7);\n"
        "    var k2 := new K(2);\n"
        "    ks.Add(k7);\n"
        "    ks.Add(k2);\n"
        "    Console.WriteLine(ks.Min().X);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1200\napple\nzoo\n2\n");
}

// fix-forward #8180 (spec §14.2 *Interface conformance*, v0.355): an element
// that conforms to `IComparable` TRANSITIVELY is not a provable violation, so
// the applicability filter KEEPS the §5 `Min()` and a user
// `extension Object { Min() }` must NEVER answer it. QUOTED rule 1: "T is a
// class that lists I (or any interface extending I) in its inheritance clause,
// that inherits from a class which conforms to I"; rule 3: "T is an interface
// that has I in its extension chain".
//
// The spec answer is the §5 `Min()` (the expected stdout below). #8185
// closed the §5.1 no-selector gate's over-reject (it read the DIRECT base
// list), so the rows now pin the §5 output ONLY: the gate's refusal is no
// longer an accepted arm. What is never admitted is the extension's 4242.
namespace {

void expectSection5MinNotTheExtension(const JitResult& r,
                                      const std::string& /*elemName*/,
                                      const std::string& expectedStdout) {
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// The INTERFACE row only: once the sema gate reads the closure (#8185), the
// element reaches codegen, whose #2770 guard (`selfComparableIface`, direct
// bases only) still refuses it -- tracked as #8186. Pinned to exactly that
// located refusal or the §5 output; the sema gate's own text and the
// extension's 4242 are no longer accepted.
void expectSection5MinOrCodegenRefusal8186(const JitResult& r,
                                           const std::string& expectedStdout) {
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "does not implement IComparable<T>"));
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
        return;
    }
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "its `CompareTo` could not be resolved for dispatch"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

// Rule 1, through an interface extending `IComparable<T>`. §5: K2 each time.
BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           class_comparable_through_an_extending_interface_never_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        std::string("public interface IOrd<T>: IComparable<T> end IOrd;\n"
                    "class K: IOrd<K>\n"
                    "  public X: int;\n"
                    "  public constructor(x: int)\n"
                    "    X := x;\n"
                    "  end;\n"
                    "  public method CompareTo(other: K): int => X - other.X;\n"
                    "end K;\n") + kObjectMinMax,
        "      var a := new Array<K>();\n"
        "      var k7 := new K(7);\n"
        "      a.Add(k7);\n"
        "      var k2 := new K(2);\n"
        "      a.Add(k2);\n"
        "      t := t + a.Min().X;\n"));
    expectSection5MinNotTheExtension(r, "K", "200\n");
}

// Rule 1, inherited from a GENERIC base class that conforms. §5: D2 each time.
BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           class_comparable_through_a_generic_base_never_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        std::string("class B<T>: IComparable<T>\n"
                    "  public X: int;\n"
                    "  public constructor(x: int)\n"
                    "    X := x;\n"
                    "  end;\n"
                    "  public virtual method CompareTo(other: T): int => 0;\n"
                    "end B;\n"
                    "class D: B<D>\n"
                    "  public constructor(x: int)\n"
                    "    base(x);\n"
                    "  end;\n"
                    "  public override method CompareTo(other: D): int =>"
                    " X - other.X;\n"
                    "end D;\n") + kObjectMinMax,
        "      var a := new Array<D>();\n"
        "      var d7 := new D(7);\n"
        "      a.Add(d7);\n"
        "      var d2 := new D(2);\n"
        "      a.Add(d2);\n"
        "      t := t + a.Min().X;\n"));
    expectSection5MinNotTheExtension(r, "D", "200\n");
}

// Rule 3, an INTERFACE element whose extension chain reaches `IComparable<T>`.
// §5: the area-1 square each time.
BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           interface_element_comparable_through_its_chain_never_answers_the_extension) {
    JitResult r = runAotChecked(looped(
        std::string("public interface IOrd<T>: IComparable<T> end IOrd;\n"
                    "public interface IShape: IOrd<IShape>\n"
                    "  method Area(): int;\n"
                    "end IShape;\n"
                    "class Sq: IShape\n"
                    "  public S: int;\n"
                    "  public constructor(s: int)\n"
                    "    S := s;\n"
                    "  end;\n"
                    "  public method Area(): int => S * S;\n"
                    "  public method CompareTo(other: IShape): int =>"
                    " Area() - other.Area();\n"
                    "end Sq;\n") + kObjectMinMax,
        "      var a := new Array<IShape>();\n"
        "      var s3 := new Sq(3);\n"
        "      a.Add(s3);\n"
        "      var s1 := new Sq(1);\n"
        "      a.Add(s1);\n"
        "      t := t + a.Min().Area();\n"));
    expectSection5MinOrCodegenRefusal8186(r, "100\n");
}

// fix #8185 controls (spec §14.2 *Interface conformance*, v0.355): the gate now
// reads the base closure, but an element that does NOT reach `IComparable` at
// all stays refused with the gate's own diagnostic, and so does one that
// implements only `IEquatable` (a different interface, not an extending one).
namespace {
void expectGateRefusal(const std::string& decls, const std::string& elem) {
    JitResult r = runAot(
        "module M;\n" + decls +
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<" + elem + ">();\n"
        "    var m := a.Min();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (elem + " does not implement IComparable<T>").c_str()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
}  // namespace

BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           class_reaching_no_icomparable_stays_refused_by_the_gate_8185) {
    expectGateRefusal(
        "public interface IOrd2 end IOrd2;\n"
        "class K: IOrd2\n"
        "  public X: int;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "  end;\n"
        "end K;\n",
        "K");
}

BEZEL_TEST(codegen_linq_icomparable_applicability_8180,
           class_implementing_only_iequatable_stays_refused_by_the_gate_8185) {
    expectGateRefusal(
        "class K: IEquatable<K>\n"
        "  public X: int;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "  end;\n"
        "  public method Equals(other: K): bool => X = other.X;\n"
        "end K;\n",
        "K");
}
