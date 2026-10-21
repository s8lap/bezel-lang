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
// FIX-FORWARD (non-author review of 3a4c8faf, MEASURED): the identity-by-
// spelling grade removed `typeRefEqual`'s false EXACT match of `Array<int>`
// with `IEnumerable<int>` (both heads declaration-less, equal arguments), and
// that false match was the only thing that had ranked a built-in collection
// argument against two instantiations of one built-in interface: the by-name
// `hasNamedAncestor` walk does not follow the collection table at all, so both
// candidates graded inapplicable and the first-declared one was rejected.
// `genericHeadReach` now grades a generic parameter by the instantiations of
// its head the argument reaches (§12.9), for built-in and user heads alike.
// Spec, quoted (v0.339):
//   §22.6: "two candidates that both require an implicit conversion at the
//     same position are **equally ranked** there"; "When a non-generic
//     candidate and a generic candidate are both applicable, and neither the
//     ranking below nor the defaulted-arity tie-break separates them, the
//     **non-generic candidate is selected**".
//   §12.9: "Type parameters without `out` or `in` are invariant." and "For
//     value-typed T, the only legal assignment of `IEnumerable<T>` is to
//     `IEnumerable<T>` itself or to `Object`."
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

// The plain AOT oracle (fix-forward rows): compile, run, expect `out`.
void expectRunsAot(const std::string& prog, const char* out) {
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

// Rejected at compile time, at a real location, with `needle` in a message.
void expectRejects(const std::string& prog, const char* needle) {
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
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

// -- fix-forward: two instantiations of ONE built-in interface. An argument
// reaches each head (`Array<T>` is an `IEnumerable<T>`, `IList<T>`, ...,
// stdlib) but converts only at its own element type: `IEnumerable<int>`
// and `IEnumerable<string>` are different types, and §12.9 admits no variant
// conversion between value-typed arguments. One candidate applies (§22.6).
// FAIL-WITHOUT (MEASURED): every row rejected with the §5.1 text.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           builtin_interface_instantiations_value_args_spec_22_6_12_9) {
    // p/i_ienum_int_str.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<int>): int => 1;
  public static method F(x: IEnumerable<string>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<int>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Array<string>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // p/i_ienum_int_long.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<int>): int => 1;
  public static method F(x: IEnumerable<long>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<int>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Array<long>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // p/i_ilist_int_long.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IList<int>): int => 1;
  public static method F(x: IList<long>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<int>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Array<long>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // p/i_irol_int_long.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IReadOnlyList<int>): int => 1;
  public static method F(x: IReadOnlyList<long>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<int>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Array<long>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // p/i_icoll_int_str.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: ICollection<int>): int => 1;
  public static method F(x: ICollection<string>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<int>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Set<string>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // u/uset_ienum.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<int>): int => 1;
  public static method F(x: IEnumerable<string>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Set<string>();
  Console.WriteLine(S.F(a));
  return 0; end Main; end P;)bzl",
                  "2\n");
    // m/n_int_nint.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<int?>): int => 1;
  public static method F(x: IEnumerable<int>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Array<int>();
  Console.WriteLine(S.F(a));
  return 0; end Main; end P;)bzl",
                  "2\n");
}

// -- fix-forward: `IList<T>` is invariant (§12.9: "Type parameters without
// `out` or `in` are invariant"), so an `Array<Dog>` is an `IList<Dog>` and
// not an `IList<Animal>`. Both declaration orders, and the Dog-only call.
// FAIL-WITHOUT (MEASURED): every row rejected with the §5.1 text.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           builtin_invariant_interface_reference_args_spec_12_9) {
    // p/c_ilist.bzl
    expectRunsAot(R"bzl(module M;
class Animal public constructor() end; end Animal;
class Dog: Animal public constructor() end; end Dog;
static class S
  public static method F(x: IList<Animal>): int => 1;
  public static method F(x: IList<Dog>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Dog>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Array<Animal>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // p/i_ilist_swap.bzl
    expectRunsAot(R"bzl(module M;
class Animal public constructor() end; end Animal;
class Dog: Animal public constructor() end; end Dog;
static class S
  public static method F(x: IList<Dog>): int => 1;
  public static method F(x: IList<Animal>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Dog>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Array<Animal>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // p/i_ilist_dog_only.bzl
    expectRunsAot(R"bzl(module M;
class Animal public constructor() end; end Animal;
class Dog: Animal public constructor() end; end Dog;
static class S
  public static method F(x: IList<Animal>): int => 1;
  public static method F(x: IList<Dog>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Dog>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "2\n");
}

// -- fix-forward: the same shape over a USER `Box<T>: IBox<T>`. `Box<int>`
// reaches `IBox` only as `IBox<int>`, so `F(IBox<string>)` is inapplicable
// to it. A covariant `IBox<out T>` still admits `Box<Dog>` at `IBox<Animal>`.
// FAIL-WITHOUT (MEASURED): both rows rejected with the §5.1 / §12.9 text.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           user_generic_interface_instantiations_spec_12_9) {
    // u/ubox.bzl
    expectRunsAot(R"bzl(module M;
interface IBox<T>
  method Get(): int;
end IBox;
class Box<T>: IBox<T>
  public constructor() end;
  public method Get(): int => 0;
end Box;
static class S
  public static method F(x: IBox<int>): int => 1;
  public static method F(x: IBox<string>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Box<int>();
  using var b := new Box<string>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(b));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // m/u_box_out.bzl
    expectRunsAot(R"bzl(module M;
class Animal public constructor() end; end Animal;
class Dog: Animal public constructor() end; end Dog;
interface IBox<out T>
  method Get(): int;
end IBox;
class Box<T>: IBox<T>
  public constructor() end;
  public method Get(): int => 0;
end Box;
static class S
  public static method F(x: IBox<Animal>): int => 1;
  public static method F(x: IBox<int>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Box<Dog>();
  using var b := new Box<int>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(b));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
}

// -- fix-forward: §22.6 "When a non-generic candidate and a generic candidate
// are both applicable, and neither the ranking below nor the defaulted-arity
// tie-break separates them, the non-generic candidate is selected." An
// `Array<int>` converts to `IEnumerable<int>` and to the generic
// `IEnumerable<V>` alike; an `Array<string>` reaches only the generic one.
// FAIL-WITHOUT (MEASURED): declared generic-first it printed 1 1, declared
// non-generic-first it was rejected with the §5.1 text.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           generic_and_non_generic_interface_candidates_spec_22_6) {
    // m/g_gen_vs_int.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F<V>(x: IEnumerable<V>): int => 1;
  public static method F(x: IEnumerable<int>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Array<int>();
  using var b := new Array<string>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(b));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // m/g_int_vs_gen.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<int>): int => 2;
  public static method F<V>(x: IEnumerable<V>): int => 1;
end S;
class P public static method Main(): int
  using var a := new Array<int>();
  using var b := new Array<string>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(b));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
}

// -- REJECT controls: an argument that reaches the head at neither candidate's
// type arguments matches no overload (§22.6 stage 7: "with no applicable
// candidate at all, the no-candidate error"). Pins that the fix grades by
// instantiation and does not admit the head alone.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           instantiation_reached_by_neither_candidate_rejected_spec_22_6) {
    // m/r_none_applies.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<int>): int => 1;
  public static method F(x: IEnumerable<string>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Array<long>();
  Console.WriteLine(S.F(a));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
    // m/u_box_reject.bzl
    expectRejects(R"bzl(module M;
interface IBox<T>
  method Get(): int;
end IBox;
class Box<T>: IBox<T>
  public constructor() end;
  public method Get(): int => 0;
end Box;
static class S
  public static method F(x: IBox<int>): int => 1;
  public static method F(x: IBox<string>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Box<long>();
  Console.WriteLine(S.F(a));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
}

// -- KEPT from the #6227 fix (non-author review, MEASURED at 3a4c8faf): each
// argument has exactly one applicable candidate across these spellings
// (`Set`, `Dictionary`, a function-typed element, a nullable outer type, a
// generic beside a non-generic candidate, a qualified and an aliased
// `Exception`, and a built-in argument beside a built-in parameter head).
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           original_fix_shapes_still_resolve_spec_22_6_12_9) {
    // p/c_set.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: Set<Exception>): int => 1;
  public static method F(x: Set<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Set<ArgumentException>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Set<Exception>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // p/c_dict.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: Dictionary<string, Exception>): int => 1;
  public static method F(x: Dictionary<string, ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Dictionary<string, ArgumentException>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Dictionary<string, Exception>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // p/f_arrfn.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: Array<(Exception) -> int>): int => 1;
  public static method F(x: Array<(ArgumentException) -> int>): int => 2;
end S;
class P public static method Main(): int
  using var g := new Array<(Exception) -> int>();
  using var h := new Array<(ArgumentException) -> int>();
  Console.WriteLine(S.F(g));
  Console.WriteLine(S.F(h));
  return 0; end Main; end P;)bzl",
                  "1\n2\n");
    // p/n_outer.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: Array<Exception>?): int => 1;
  public static method F(x: Array<ArgumentException>?): int => 2;
end S;
class P public static method Main(): int
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(e));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // p/g_tvsae.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F<T>(x: Array<T>): int => 1;
  public static method F(x: Array<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Array<ArgumentException>();
  using var b := new Array<Exception>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(b));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // p/i_set_ienum.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<int>): int => 1;
  public static method F(x: Set<int>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Set<int>();
  Console.WriteLine(S.F(v0));
  using var v1 := new Array<int>();
  Console.WriteLine(S.F(v1));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // p/q_sys.bzl
    expectRunsAot(R"bzl(module M;
static class S
  public static method F(x: Array<System.Exception>): int => 1;
  public static method F(x: Array<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(e));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
    // p/q_alias_pair.bzl
    expectRunsAot(R"bzl(module M;
type EX := Exception;
static class S
  public static method F(x: Array<EX>): int => 1;
  public static method F(x: Array<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(e));
  return 0; end Main; end P;)bzl",
                  "2\n1\n");
}

// -- KEPT from the #6227 fix: an override pair, an override reached through
// the base, a three-level chain, and an interface pair implemented by a
// class. Both overloads are members (§11.3) and each call picks its own.
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           original_fix_hierarchy_shapes_still_resolve_spec_22_6_12_2) {
    // b/o2_both.bzl
    expectRunsAot(R"bzl(module M;
class B public constructor() end;
  public virtual method F(x: Array<Exception>): int => 1;
  public virtual method F(x: Array<ArgumentException>): int => 2;
end B;
class D: B public constructor() end;
  public override method F(x: Array<Exception>): int => 11;
  public override method F(x: Array<ArgumentException>): int => 12;
end D;
class P public static method Main(): int
  using var r := new D();
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(r.F(a));
  Console.WriteLine(r.F(e));
  return 0; end Main; end P;)bzl",
                  "12\n11\n");
    // b/o2_viaB.bzl
    expectRunsAot(R"bzl(module M;
class B public constructor() end;
  public virtual method F(x: Array<Exception>): int => 1;
  public virtual method F(x: Array<ArgumentException>): int => 2;
end B;
class D: B public constructor() end;
  public override method F(x: Array<ArgumentException>): int => 12;
end D;
class P public static method Main(): int
  using var d := new D();
  var r: B := d;
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(r.F(a));
  Console.WriteLine(r.F(e));
  return 0; end Main; end P;)bzl",
                  "12\n1\n");
    // b/o3_both.bzl
    expectRunsAot(R"bzl(module M;
class B public constructor() end;
  public virtual method F(x: Array<Exception>): int => 1;
  public virtual method F(x: Array<ArgumentException>): int => 2;
end B;
class D: B public constructor() end;
  public override method F(x: Array<ArgumentException>): int => 12;
  public override method F(x: Array<Exception>): int => 11;
end D;
class G: D public constructor() end;
  public override method F(x: Array<Exception>): int => 21;
  public override method F(x: Array<ArgumentException>): int => 22;
end G;
class P public static method Main(): int
  using var r := new G();
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(r.F(a));
  Console.WriteLine(r.F(e));
  return 0; end Main; end P;)bzl",
                  "22\n21\n");
    // b/iabs_impl.bzl
    expectRunsAot(R"bzl(module M;
interface I
  method F(x: Array<Exception>): int;
  method F(x: Array<ArgumentException>): int;
end I;
class C: I public constructor() end;
  public method F(x: Array<ArgumentException>): int => 12;
  public method F(x: Array<Exception>): int => 11;
end C;
class P public static method Main(): int
  using var r := new C();
  var i: I := r;
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(r.F(a));
  Console.WriteLine(r.F(e));
  Console.WriteLine(i.F(a));
  Console.WriteLine(i.F(e));
  return 0; end Main; end P;)bzl",
                  "12\n11\n12\n11\n");
}

// -- KEPT from the #6227 fix: `null` at two nullable instantiations is
// ambiguous (§22.6: a `null` literal "is graded alike at every candidate it
// reaches, so it separates none"), and an argument naming a third
// instantiation of an invariant head matches neither candidate (§12.9).
// `r_ienum_ioe` (an `Array<InvalidOperationException>` at
// `IEnumerable<Exception>`) is deliberately NOT pinned: §12.9 makes
// `IEnumerable<out T>` covariant over reference types, so its rejection is
// a defect, not a contract (filed separately).
BEZEL_TEST(sema_overload_builtin_generic_argument_resolve_6227,
           original_fix_ambiguity_and_rejects_kept_spec_22_6_12_9) {
    // p/n_outer_null.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: Array<Exception>?): int => 1;
  public static method F(x: Array<ArgumentException>?): int => 2;
end S;
class P public static method Main(): int
  Console.WriteLine(S.F(null));
  return 0; end Main; end P;)bzl",
                  "ambiguous call to `F`");
    // p/r_arr_ioe.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: Array<Exception>): int => 1;
  public static method F(x: Array<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<InvalidOperationException>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
    // p/r_arr_obj.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: Array<Exception>): int => 1;
  public static method F(x: Array<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Object>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
    // p/r_dict_obj.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: Dictionary<string, Exception>): int => 1;
  public static method F(x: Dictionary<string, ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Dictionary<string, Object>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
    // p/r_ienum_obj.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: IEnumerable<Exception>): int => 1;
  public static method F(x: IEnumerable<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Object>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
    // p/r_nest_short.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: Array<Array<int>>): int => 1;
  public static method F(x: Array<Array<long>>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Array<short>>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
    // p/r_set_obj.bzl
    expectRejects(R"bzl(module M;
static class S
  public static method F(x: Set<Exception>): int => 1;
  public static method F(x: Set<ArgumentException>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Set<Object>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
    // p/r_user_obj.bzl
    expectRejects(R"bzl(module M;
class Animal public constructor() end; end Animal;
class Dog: Animal public constructor() end; end Dog;
static class S
  public static method F(x: Array<Animal>): int => 1;
  public static method F(x: Array<Dog>): int => 2;
end S;
class P public static method Main(): int
  using var v0 := new Array<Object>();
  Console.WriteLine(S.F(v0));
  return 0; end Main; end P;)bzl",
                  "has a type that is not implicitly convertible to the target type");
}
