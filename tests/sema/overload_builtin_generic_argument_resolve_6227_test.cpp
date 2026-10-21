// Spec-derived regression tests for issue #6227 -- overloads that differ only
// in the BUILT-IN class named inside a generic argument (`F(Array<Exception>)`
// beside `F(Array<ArgumentException>)`) were accepted as declarations (#6335)
// but every CALL was rejected: "the argument `x` to call to `F` has a type
// that is not implicitly convertible to the target type (§5.1)".
//
// Spec (docs/Bezel_Language_Spec.md, v0.339):
//   §11.3: two methods clash only when they "do not differ in number of
//     parameters, number of type parameters of their own, parameter types, or
//     the `ref`/`out` modifiers" -- `Array<Exception>` and
//     `Array<ArgumentException>` differ in parameter type, so both overloads
//     are members and both are candidates.
//   §12.9: `Array` is invariant -- `Array<ArgumentException>` does NOT convert
//     to `Array<Exception>`. So each argument has exactly ONE applicable
//     candidate, the one naming its own instantiation.
//   §22.6: overload resolution picks the single applicable candidate.
//
// ROOT CAUSE (MEASURED, debug traces at staging 9ae432f0): every built-in
// class ref carries `declaration == nullptr`, and the declaration-blind
// `typeRefEqual` equates `Array<Exception>` with `Array<ArgumentException>`.
//   (1) `collectHierarchyOverloads`' `shadows` dedup dropped the second
//       same-class overload as a "shadowed" duplicate, leaving ONE candidate,
//       so the argument naming the other instantiation was not convertible.
//   (2) with (1) fixed, the Undecidable arm's `builtinRefPrefers` grade and
//       the #6332 non-generic admission still called both candidates exact.
//
// FAIL-WITHOUT: every run row below is rejected with the §5.1 text on the
// pre-fix compiler. The order-swapped rows catch a fix that falls back to
// first-declared.
//
// Not covered (unchanged by this fix, filed separately): the CONSTRUCTOR
// spelling `constructor(x: Array<Exception>)` / `constructor(x:
// Array<ArgumentException>)`, which routes through codegen's `ctorForArgs` and
// picks the first-declared constructor whatever the argument.

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

// `decls` is the two overload declarations, `recv` the call receiver prefix
// (`S.` or `c.`), `setup` any receiver construction.
std::string prog(const std::string& head, const std::string& decls,
                 const std::string& tail, const std::string& setup,
                 const std::string& recv) {
    return "module M; " + head + decls + tail +
           "class P public static method Main(): int "
           "  " + setup +
           "  using var a := new Array<ArgumentException>(); "
           "  using var e := new Array<Exception>(); "
           "  Console.WriteLine(" + recv + "F(a)); "
           "  Console.WriteLine(" + recv + "F(e)); "
           "  return 0; end Main; end P;";
}

const char* kExFirst =
    "  public static method F(x: Array<Exception>): int => 1; "
    "  public static method F(x: Array<ArgumentException>): int => 2; ";
const char* kAeFirst =
    "  public static method F(x: Array<ArgumentException>): int => 2; "
    "  public static method F(x: Array<Exception>): int => 1; ";

std::string instanceDecls(const char* s) {
    std::string d = s;
    for (std::string::size_type p; (p = d.find("static ")) != std::string::npos;)
        d.erase(p, 7);
    return d;
}

}  // namespace

// -- The issue's exact repro: a literal-initialised `Array<ArgumentException>`
// local picks the `Array<ArgumentException>` overload (§22.6, §12.9).
// Run on the plain AOT oracle, not the checked one: the literal's fresh
// element leaks under LSan whatever overload is called (MEASURED with a single
// non-overloaded `F` too) -- that is the pre-existing open #2676, not this bug.
// Every other row binds `new Array<...>()` and runs under the checked oracle.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           issue_repro_static_picks_own_instantiation_spec_22_6) {
    auto r = runAot(
        "module M; "
        "static class S "
        "  public static method F(x: Array<Exception>): int => 1; "
        "  public static method F(x: Array<ArgumentException>): int => 2; "
        "end S; "
        "class P public static method Main(): int "
        "  var a: Array<ArgumentException> := [new ArgumentException(\"a\")]; "
        "  Console.WriteLine(S.F(a)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// -- Both arguments, both declaration orders, on a static class.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           static_both_orders_spec_22_6_12_9) {
    expectRuns(prog("static class S ", kExFirst, "end S; ", "", "S."),
               "2\n1\n");
    expectRuns(prog("static class S ", kAeFirst, "end S; ", "", "S."),
               "2\n1\n");
}

// -- The reviewer's r2 shape: instance methods, AE overload declared first.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           reviewer_r2_instance_methods_spec_22_6) {
    expectRuns(
        "module M; "
        "class C "
        "  public constructor() end; "
        "  public method F(x: Array<ArgumentException>): int => 4; "
        "  public method F(x: Array<Exception>): int => 3; "
        "end C; "
        "class P public static method Main(): int "
        "  using var c := new C(); "
        "  using var e := new Array<Exception>(); "
        "  using var g := new Array<ArgumentException>(); "
        "  Console.WriteLine(c.F(e).ToString() + c.F(g).ToString()); "
        "  return 0; end Main; end P;",
        "34\n");
}

// -- Instance methods, both declaration orders.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           instance_both_orders_spec_22_6_12_9) {
    const std::string setup = "using var c := new C(); ";
    expectRuns(prog("class C public constructor() end; ",
                    instanceDecls(kExFirst), "end C; ", setup, "c."),
               "2\n1\n");
    expectRuns(prog("class C public constructor() end; ",
                    instanceDecls(kAeFirst), "end C; ", setup, "c."),
               "2\n1\n");
}

// -- An extension block's pair (§26.5 members ranked by §22.6).
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           extension_both_orders_spec_26_5_22_6) {
    const std::string setup = "using var c := new C(); ";
    const std::string head = "class C public constructor() end; end C; extension C ";
    expectRuns(prog(head, instanceDecls(kExFirst), "end; ", setup, "c."),
               "2\n1\n");
    expectRuns(prog(head, instanceDecls(kAeFirst), "end; ", setup, "c."),
               "2\n1\n");
}

// -- A sibling pair under `Exception` and a nullable-element pair: each
// argument still has exactly one applicable candidate.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           sibling_and_nullable_element_pairs_spec_12_9) {
    expectRuns(
        "module M; static class S "
        "  public static method F(x: Array<InvalidOperationException>): int => 1; "
        "  public static method F(x: Array<ArgumentException>): int => 2; "
        "end S; "
        "class P public static method Main(): int "
        "  using var a := new Array<ArgumentException>(); "
        "  using var i := new Array<InvalidOperationException>(); "
        "  Console.WriteLine(S.F(a)); Console.WriteLine(S.F(i)); "
        "  return 0; end Main; end P;",
        "2\n1\n");
    expectRuns(
        "module M; static class S "
        "  public static method F(x: Array<ArgumentException?>): int => 2; "
        "  public static method F(x: Array<Exception?>): int => 1; "
        "end S; "
        "class P public static method Main(): int "
        "  using var a := new Array<ArgumentException?>(); "
        "  using var e := new Array<Exception?>(); "
        "  Console.WriteLine(S.F(a)); Console.WriteLine(S.F(e)); "
        "  return 0; end Main; end P;",
        "2\n1\n");
}

// -- REJECT (§12.9 invariance): an `Array<Object>` argument matches neither
// overload. Pins that the fix did not make instantiations interconvertible.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           unrelated_instantiation_still_rejected_spec_12_9) {
    auto r = runAotChecked(
        "module M; static class S " + std::string(kExFirst) + "end S; "
        "class P public static method Main(): int "
        "  using var o := new Array<Object>(); "
        "  Console.WriteLine(S.F(o)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has a type that is not implicitly convertible to the target type"));
}
