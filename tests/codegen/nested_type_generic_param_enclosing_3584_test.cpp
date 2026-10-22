// fix #3584 — nested type declared inside a generic enclosing class that
// USES the enclosing class's type parameter (`class Outer<T> { class Tag {
// _v: T } }`) silently collapsed to a single instantiation and reinterpreted
// the payload at runtime (the issue's repro: `new Outer.Tag(7)` worked,
// `new Outer.Tag("hello")` printed `4423056`, a pointer-shaped int). The
// unspecialized layout used the i32 generic-parameter placeholder for the
// nested type's `_v: T` field, so the first instantiation's layout was the
// one every subsequent store silently corrupted.
//
// Spec citations (v0.286): §11.6 (nested types — silent corruption is not an
// admissible outcome of a type declaration); §14.1 (generic declarations — a
// nested type that references an enclosing class's type parameter is
// implicitly generic, with no spec-admitted spelling); §2.8.3 rule 3 (type
// identity — `Outer<int>.Tag` and `Outer<string>.Tag` MUST be distinct
// constructions, not one). The local-type analogue (§21.10 restrictions) is
// "a local type may not reference an enclosing generic parameter" — that is
// the closest spec-adjacent rule and the precedent the new diagnostic
// follows.
//
// The fix lives in `Resolver::resolveNamed` (src/sema/TypeResolution_Names.cpp).
// When a bare name inside a nested type's body resolves to a generic
// parameter through a parent scope, the new check asks: "is this the
// current type's own parameter, or the enclosing type's?". The enclosing
// case is a compile error naming the rule and a remedy. Two instantiations
// of the same shape (one with `int`, one with `string`) are caught at the
// declaration site by the same diagnostic, not collapsed into one
// unspecialized layout at codegen time.
//
// This test pins:
//   * PART 1: the bug is REJECTED at compile time, not silently corrupted at
//     runtime. The diagnostic names §11.6 / §14.1 and the offending name. A
//     two-instantiation variant (one `int`, one `string`) is the same
//     violation — a single rule, two distinct construction shapes.
//   * PART 2: the silent-corruption shape (the issue's literal repro) is
//     REJECTED, not COMPILED. A program that compiled and printed garbage
//     is no longer admissible.
//   * PART 3: the over-rejection guards — every shape #3582 PART 3 pinned
//     still compiles and runs. The new diagnostic does not reach:
//        - a nested type that does NOT use the enclosing's parameters
//          (returns the constructor's int — same row #3582 PART 3 covers)
//        - a non-nested generic (`Box<int>`)
//        - a generic method on a non-nested type
//     These were the working programs on the pre-fix tree; they stay
//     working on the post-fix tree.

#include <string>

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff the program is REJECTED and some diagnostic contains `needle`.
// Both halves matter: `hasErrors()` alone would pass on an unrelated failure,
// and a matching string on a program that still compiled would prove nothing.
bool rejects(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// §11.6 / #3584: a nested type declared inside a GENERIC enclosing class
// whose body uses the enclosing class's type parameter. The shape the issue
// reports — `class Outer<T> { class Tag { _v: T } }`. `classStruct` used to
// return the unspecialized (i32 placeholder) layout shared by every
// instantiation, so two `new Outer.Tag(...)` calls with different `T`s
// silently corrupted the second one's payload.
const char* kOuterWithTagUsingT =
    "public class Outer<T> "
    "  public class Tag "
    "    private _v: T; "
    "    public constructor(v: T) _v := v; end constructor; "
    "    public method V(): T => _v; "
    "  end Tag; "
    "end Outer; ";

// The same shape with the nested type NOT using the enclosing's parameters —
// the working case the over-rejection guard has to keep working. Mirrors
// #3582 PART 3 so the relationship to that fix is obvious.
const char* kOuterWithTagNotUsingT =
    "public class Outer<T> "
    "  public class Tag "
    "    private _n: int; "
    "    public constructor(n: int) _n := n; end constructor; "
    "    public method N(): int => _n; "
    "  end Tag; "
    "  public static method Make(n: int): owned Tag return new Tag(n); end Make; "
    "end Outer; ";

std::string withOuter(const std::string& outerDecl,
                      const std::string& stmts) {
    return "module M; " + std::string(outerDecl) +
           "public class C public static method Main(): int " + stmts +
           " end Main; end C;";
}

}  // namespace

// =============================================================================
// PART 1 — the bug is REJECTED. Two instantiations of the issue's shape must
// each fail to compile with a diagnostic naming the rule and the offending
// generic parameter. The two-instantiation variant is the issue's own repro:
// one `Outer.Tag(7)` and one `Outer.Tag("hello")`. Pre-fix: both compile and
// the second prints garbage. Post-fix: both fail with the same diagnostic at
// the declaration site.
// =============================================================================

BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           tag_using_enclosing_param_is_rejected_spec_11_6) {
    // A SINGLE instantiation — already a violation; the diagnostic fires
    // at the declaration site, not the construction site, because the bug
    // is the nested type's BODY silently using T.
    const std::string p = withOuter(
        kOuterWithTagUsingT,
        "using var a: Outer.Tag := new Outer.Tag(7); return a.V();");
    BEZEL_EXPECT_TRUE(rejects(p, "nested type `Tag` references type parameter "
                                  "`T` of its enclosing type `Outer.1`"));
    BEZEL_EXPECT_TRUE(rejects(p, "\xc2\xa7""11.6"));
    BEZEL_EXPECT_TRUE(rejects(p, "\xc2\xa7""14.1"));
}

BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           two_distinct_instantiations_rejected_spec_11_6) {
    // The issue's literal repro: `new Outer.Tag(7)` followed by
    // `new Outer.Tag("hello")`. Pre-fix both compile and the second prints
    // garbage. Post-fix the program is a compile error at the DECLARATION
    // site — the nested type's `_v: T` is the violation, not the
    // construction calls.
    const std::string p = withOuter(
        kOuterWithTagUsingT,
        "using var a: Outer.Tag := new Outer.Tag(7); "
        "using var b: Outer.Tag := new Outer.Tag(\"hello\"); "
        "Console.WriteLine(b.V()); "
        "return a.V();");
    BEZEL_EXPECT_TRUE(rejects(p, "nested type `Tag` references type parameter "
                                  "`T` of its enclosing type `Outer.1`"));
    // The diagnostic must NOT silently let the program through to runtime.
    BEZEL_EXPECT_FALSE(run(p) == 4423056);  // pre-fix garbage value
}

// =============================================================================
// PART 2 — the silent-corruption SHAPE is REJECTED. The bare field type `T`
// inside Tag — exactly what the issue says the compiler used to accept and
// then silently corrupt at runtime. A `private _v: T;` declaration must
// reject, not compile-and-corrupt.
// =============================================================================

BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           field_using_enclosing_param_rejected_spec_11_6) {
    const std::string p = withOuter(
        "public class Outer<T> "
        "  public class Tag "
        "    private _v: T; "
        "  end Tag; "
        "end Outer; ",
        "using var a: Outer.Tag := new Outer.Tag(7); return 0;");
    BEZEL_EXPECT_TRUE(rejects(p, "nested type `Tag` references type parameter "
                                  "`T` of its enclosing type `Outer.1`"));
}

BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           method_return_using_enclosing_param_rejected_spec_11_6) {
    // The return type `T` of a method on the nested type is also the
    // enclosing's parameter — caught by the same diagnostic.
    const std::string p = withOuter(
        "public class Outer<T> "
        "  public class Tag "
        "    public method V(): T => default(T); "
        "  end Tag; "
        "end Outer; ",
        "using var a: Outer.Tag := new Outer.Tag(7); return 0;");
    BEZEL_EXPECT_TRUE(rejects(p, "nested type `Tag` references type parameter "
                                  "`T` of its enclosing type `Outer.1`"));
}

// =============================================================================
// PART 3 — over-rejection guards. The working programs the diagnostic must
// NOT touch. Each is pinned on a produced VALUE, not on "it compiled".
// =============================================================================

// §11.6 / #3582 PART 3 baseline: a nested type inside a generic enclosing
// class that does NOT use the enclosing's parameters. The diagnostic must
// NOT reach this shape.
// fix #6965 review (§11.6, v0.340; ruling #5386): respelled. The row's old
// `using var t: Outer.Tag := new Outer.Tag(7);` is Appendix C.1's arity error
// at a generic-only `Outer` — "A bare enclosing segment fixes arity 0 ... In
// a type position, therefore, no name passes through a generic enclosing
// type" — and §11.6's route is kept: "Elsewhere a chain head reaches its
// static members, and a local declared without a type annotation holds a
// value of it".
BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           nested_type_without_enclosing_params_still_runs_spec_11_6) {
    BEZEL_EXPECT_EQ(
        run(withOuter(kOuterWithTagNotUsingT,
                      "using var t := Outer<int>.Make(7); "
                      "return t.N();")),
        7);
}

// A nested type that uses the enclosing's params via a generic method's OWN
// parameter (`class Tag { method Foo<U>() { ... use U ... } }`) — `U` is
// the METHOD's parameter, not the enclosing's, so this is the ordinary
// nested-method-generic shape. The diagnostic must NOT reach it.
BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           nested_type_own_generic_method_still_runs_spec_14_1) {
    const std::string p = withOuter(
        "public class Outer<T> "
        "  public class Tag "
        "    public static method Id<U>(x: U): U => x; "
        "  end Tag; "
        "end Outer; ",
        "return Outer<int>.Tag.Id<int>(5);");
    BEZEL_EXPECT_EQ(run(p), 5);
}

// §14.1 baseline: an ordinary non-nested generic in an ascription. The
// diagnostic must NOT reach it.
BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           plain_constructed_generic_still_runs_spec_14_1) {
    BEZEL_EXPECT_EQ(
        run("module M; public class Box<T> "
            "  private _v: int; "
            "  public constructor(v: int) _v := v; end constructor; "
            "  public method V(): int => _v; "
            "end Box; "
            "public class C public static method Main(): int "
            "  using var b: Box<int> := new Box<int>(9); "
            "  return b.V(); "
            "end Main; end C;"),
        9);
}

// A nested type inside a NON-generic enclosing class — the enclosing has no
// parameters, so there is nothing for the nested type to "reference". The
// diagnostic must NOT reach this shape.
BEZEL_TEST(nested_type_generic_param_enclosing_3584,
           nested_type_in_nongeneric_outer_still_runs_spec_11_6) {
    BEZEL_EXPECT_EQ(
        run("module M; public class Outer "
            "  public class Tag "
            "    private _n: int; "
            "    public constructor(n: int) _n := n; end constructor; "
            "    public method N(): int => _n; "
            "  end Tag; "
            "end Outer; "
            "public class C public static method Main(): int "
            "  using var t: Outer.Tag := new Outer.Tag(3); "
            "  return t.N(); "
            "end Main; end C;"),
        3);
}
