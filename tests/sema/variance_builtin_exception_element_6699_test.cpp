// Spec-derived regression tests for issue #6699 -- `Array<E>`, with `E` a
// class derived from the built-in `Exception`, was not covariantly
// convertible to `IEnumerable<Exception>` / `IReadOnlyList<Exception>`:
// "the initializer for `e` has a type that is not implicitly convertible to
// the target type (§5.1)". The same shape over user classes
// (`Array<Dog>` -> `IEnumerable<Animal>`) was accepted.
//
// Spec (docs/Bezel_Language_Spec.md, v0.339):
//   §12.9: "Reference-typed T admits the full covariant chain —
//     `IEnumerable<Dog>` is assignable to `IEnumerable<Animal>` and to
//     `IEnumerable<Object>` because the underlying references compose without
//     conversion. The relation a variant conversion asks of the two arguments
//     is a reference conversion ([§5.1]), its lift to the nullable forms
//     (`Dog?` to `Animal?`, ...), or the nullable view of a reference type
//     (`string` to `string?`), composed (`Dog` to `Animal?`): under `out`,
//     `I<S>` converts to `I<T>` when `S` so converts to `T`; under `in`, when
//     `T` so converts to `S`. No other conversion takes part, and the nullable
//     view's reverse never does".
//   §5.1 (Reference conversions): "**class to base class**, transitively, up
//     to `Object`".
//
// ROOT CAUSE (MEASURED): `typeArgCovariantAssignable` related two element
// types only by identity, `Object`, a shared head, the built-in COLLECTION
// table, or `isSubtype` between two TypeSymbols. A built-in exception class
// has no TypeSymbol, so `ArgumentException` -> `Exception` (both built-in)
// found no collection ancestor, and `MyErr` -> `Exception` (user -> built-in)
// hit the "mixed base: no known relation" reject.
//
// FAIL-WITHOUT: every run row below is rejected with the §5.1 text on the
// pre-fix compiler. The reject rows pass both ways (KEPT controls).
//
// Not pinned here: `F(IEnumerable<Exception>)` beside
// `F(IEnumerable<ArgumentException>)` with an `Array<ArgumentException>`
// argument. Both candidates apply, so §22.6 makes it ambiguous; it now
// silently selects the first-declared candidate, which is open #6702.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Compile, run under the checked (ASan) oracle, and expect `out` on stdout.
void expectRuns(const std::string& prog, const char* out) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

void expectRejects(const std::string& prog, const char* needle) {
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

const char* kNotConvertible =
    "is not implicitly convertible to the target type (§5.1)";

// `Array<ArgumentException>` viewed as `Array<Exception>`-family targets.
std::string rejectProg(const char* elem, const char* target,
                       const char* decls = "") {
    return std::string("module M; ") + decls +
           "class P public static method Main(): int "
           "  using var a := new Array<" + elem + ">(); "
           "  var e: " + target + " := a; "
           "  return 0; end Main; end P;";
}

}  // namespace

// -- The issue's repro, iterated: both views read the borrowed elements.
BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           issue_repro_builtin_subclass_elements_spec_12_9) {
    expectRuns(
        "module M; "
        "class P public static method Main(): int "
        "  using var x := new ArgumentException(\"ax\"); "
        "  using var y := new ArgumentException(\"ay\"); "
        "  using var a := new Array<ArgumentException>(); "
        "  a.Add(x); a.Add(y); "
        "  var e: IEnumerable<Exception> := a; "
        "  var r: IReadOnlyList<Exception> := a; "
        "  for var v in e do Console.WriteLine(v.Message); end; "
        "  Console.WriteLine(r.Count); "
        "  Console.WriteLine(r[1].Message); "
        "  return 0; end Main; end P;",
        "ax\nay\n2\nay\n");
}

// -- A user subclass of `Exception`, one level and two levels deep, and a
// view at the intermediate user class (§5.1 "transitively").
BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           user_exception_subclass_elements_spec_12_9_5_1) {
    expectRuns(
        "module M; "
        "class MyErr: Exception "
        "  public constructor(m: string) base(m); end; "
        "end MyErr; "
        "class MyErr2: MyErr "
        "  public constructor(m: string) base(m); end; "
        "end MyErr2; "
        "class P public static method Main(): int "
        "  using var x := new MyErr(\"m1\"); "
        "  using var y := new MyErr2(\"m2\"); "
        "  using var a := new Array<MyErr>(); "
        "  a.Add(x); "
        "  using var b := new Array<MyErr2>(); "
        "  b.Add(y); "
        "  var e: IEnumerable<Exception> := a; "
        "  var f: IReadOnlyList<Exception> := b; "
        "  var g: IEnumerable<MyErr> := b; "
        "  for var v in e do Console.WriteLine(v.Message); end; "
        "  for var v in f do Console.WriteLine(v.Message); end; "
        "  for var v in g do Console.WriteLine(v.Message); end; "
        "  return 0; end Main; end P;",
        "m1\nm2\nm2\n");
}

// -- Two built-in levels (`ArgumentNullException` -> `ArgumentException` ->
// `Exception`), and the nullable view `Exception?` of the target (§12.9).
BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           builtin_two_levels_and_nullable_view_spec_12_9) {
    expectRuns(
        "module M; "
        "class P public static method Main(): int "
        "  using var x := new ArgumentNullException(\"pn\"); "
        "  using var a := new Array<ArgumentNullException>(); "
        "  a.Add(x); "
        "  var e: IEnumerable<Exception> := a; "
        "  var f: IEnumerable<ArgumentException> := a; "
        "  var g: IEnumerable<Exception?> := a; "
        "  for var v in e do Console.WriteLine(v.Message); end; "
        "  for var v in f do Console.WriteLine(v.Message); end; "
        "  for var v in g do Console.WriteLine(v!.Message); end; "
        "  return 0; end Main; end P;",
        "pn\npn\npn\n");
}

// -- A `return` and a single-candidate (no overload) call.
BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           return_and_single_candidate_call_spec_12_9) {
    expectRuns(
        "module M; "
        "static class S "
        "  public static method View(a: Array<InvalidOperationException>): "
        "IEnumerable<Exception> => a; "
        "  public static method Count(xs: IReadOnlyList<Exception>): int "
        "    var n := 0; "
        "    for var v in xs do Console.WriteLine(v.Message); n := n + 1; end; "
        "    return n; "
        "  end Count; "
        "end S; "
        "class P public static method Main(): int "
        "  using var x := new InvalidOperationException(\"io\"); "
        "  using var a := new Array<InvalidOperationException>(); "
        "  a.Add(x); "
        "  for var v in S.View(a) do Console.WriteLine(v.Message); end; "
        "  Console.WriteLine(S.Count(a)); "
        "  return 0; end Main; end P;",
        "io\nio\n1\n");
}

// -- User-declared `out` and `in` interfaces over built-in exceptions.
BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           user_variant_interfaces_spec_12_9) {
    expectRuns(
        "module M; "
        "interface ISrc<out T> method Get(): T; end ISrc; "
        "interface ISink<in T> method Put(x: T): int; end ISink; "
        "class Src: ISrc<ArgumentException> "
        "  private _e: ArgumentException; "
        "  public constructor(e: ArgumentException) _e := e; end; "
        "  public method Get(): ArgumentException => _e; "
        "end Src; "
        "class Sink: ISink<Exception> "
        "  public constructor() end; "
        "  public method Put(x: Exception): int "
        "    Console.WriteLine(x.Message); "
        "    return 7; "
        "  end Put; "
        "end Sink; "
        "class P public static method Main(): int "
        "  using var x := new ArgumentException(\"sx\"); "
        "  using var s := new Src(x); "
        "  using var k := new Sink(); "
        "  var a: ISrc<Exception> := s; "
        "  var b: ISink<ArgumentException> := k; "
        "  Console.WriteLine(a.Get().Message); "
        "  Console.WriteLine(b.Put(x)); "
        "  return 0; end Main; end P;",
        "sx\nsx\n7\n");
}

// -- §22.6 with exactly one applicable candidate: `Array<InvalidOperation
// Exception>` converts to `IEnumerable<Exception>` only.
BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           overload_one_applicable_candidate_spec_22_6_12_9) {
    expectRuns(
        "module M; "
        "static class S "
        "  public static method F(x: IEnumerable<Exception>): int => 1; "
        "  public static method F(x: IEnumerable<ArgumentException>): int => 2; "
        "end S; "
        "class P public static method Main(): int "
        "  using var a := new Array<InvalidOperationException>(); "
        "  using var e := new Array<Exception>(); "
        "  Console.WriteLine(S.F(a)); "
        "  Console.WriteLine(S.F(e)); "
        "  return 0; end Main; end P;",
        "1\n1\n");
}

// -- KEPT controls: conversions §12.9 / §5.1 do not admit.
BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           rejects_downcast_invariant_nullable_reverse_spec_12_9) {
    // base-to-derived element
    expectRejects(rejectProg("Exception", "IEnumerable<ArgumentException>"),
                  kNotConvertible);
    // `Array` is invariant
    expectRejects(rejectProg("ArgumentException", "Array<Exception>"),
                  kNotConvertible);
    // the nullable view's reverse
    expectRejects(rejectProg("ArgumentException?", "IEnumerable<Exception>"),
                  kNotConvertible);
    // sibling built-in exceptions
    expectRejects(rejectProg("ArgumentException",
                             "IEnumerable<InvalidOperationException>"),
                  kNotConvertible);
}

BEZEL_TEST(sema_variance_builtin_exception_element_6699,
           rejects_unrelated_user_and_builtin_classes_spec_12_9) {
    const char* myErr =
        "class MyErr: Exception "
        "  public constructor(m: string) base(m); end; "
        "end MyErr; ";
    const char* animal = "class Animal public constructor() end; end Animal; ";
    expectRejects(rejectProg("MyErr", "IEnumerable<ArgumentException>", myErr),
                  kNotConvertible);
    expectRejects(rejectProg("Animal", "IEnumerable<Exception>", animal),
                  kNotConvertible);
    expectRejects(rejectProg("ArgumentException", "IEnumerable<Animal>", animal),
                  kNotConvertible);
    // `in`: `ISink<ArgumentException>` does not convert to `ISink<Exception>`.
    expectRejects(
        "module M; "
        "interface ISink<in T> method Put(x: T): int; end ISink; "
        "class Sink: ISink<ArgumentException> "
        "  public constructor() end; "
        "  public method Put(x: ArgumentException): int => 7; "
        "end Sink; "
        "class P public static method Main(): int "
        "  using var k := new Sink(); "
        "  var b: ISink<Exception> := k; "
        "  return 0; end Main; end P;",
        kNotConvertible);
}
