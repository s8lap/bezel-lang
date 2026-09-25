// Spec-derived regression test for the #4895 REWORK-REDO.
//
// (spec §14.5 phase 1 + phase 3, §15.4, v0.307).
//
// THE ORIGINAL BUG (#4895, commit 335e3849). A generic method's type
// parameter T whose only source was a heterogeneous collection literal
// was silently bound by the binder's element-by-element `checkAssignable`
// walk: T bound to the first element's resolved type, the second
// element's type failed against that first-element type, but the call
// still typechecked and codegen proceeded with T wrong. Measured on
// `Show(["s", new A()])`: ran clean, no diagnostics, `4435344|381342640`
// at runtime — T bound to `string`, the second element `new A()`
// reinterpreted through the string representation.
//
// THE FIRST REWORK (commit 3f51d766, branch 4895-rework-v2). Narrowed
// the predicate so that T gets bound by ANY non-literal argument's static
// type, fixing sound programs like `Pick([], 7)` (T bound by `7`) and
// `Count(xs, [])` with `xs: Array<int>` (T bound by `xs`). But opened
// a NEW silent-crash class on heterogeneous-literal-with-binding-source:
// `F<T>(a: Array<T>, b: Array<T>)` called as `F(["s", 1], xs)` with
// `xs: Array<int>` printed `v=4435280` (the string `"s"` silently
// treated as int via the binder's element-by-element coercion against
// the now-bound T=int).
//
// THE REWORK-REDO (this test). Adds a heterogeneous-literal classifier
// that runs alongside the narrowing predicate. A heterogeneous literal
// cannot bind T (its elements do not share a single TypeRef) AND cannot
// be safely coerced against T once T is determined (the binder coerces
// elements without re-checking the literal's element types), so the
// predicate rejects when ANY visited collection literal is
// heterogeneous. Empty literals are NOT heterogeneous (no element to
// coerce); homogeneous literals are NOT heterogeneous (binder's
// element-by-element walk works). Explicit `<T>` short-circuits the
// outer `!hadExplicitTypeArgs` guard, so `Show<int>(["s", 1])` is
// accepted regardless.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff some emitted diagnostic mentions the §14.5 phase-3
// collection-literal-only-source inference failure (the rework-redo's
// diagnostic).
bool has4895ReworkRedoDiag(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("phase 3 of §14.5") != std::string::npos &&
            d.message.find("do not all share a single determined type") !=
                std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasAnyError(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// REJECT — the new heterogeneous-literal classifier (rework-redo).
// FAIL-WITHOUT proof on the pre-fix 335e3849 over-broad predicate:
// D1/D2/D3 are REJECTED with the original over-broad §14.5 phase-3
// diagnostic. Post-rework-redo they MUST STILL BE REJECTED, but with the
// NEW heterogeneous-literal diagnostic text (the diff between the two
// is the property the fix achieves).
// ===========================================================================

// D1 (CRITICAL): `F<T>(a: Array<T>, b: Array<T>): ()` called as
// `F(["s", 1], xs)` with `xs: Array<int>`. The first arg is a
// heterogeneous literal (`["s", 1]` — string and int are different
// TypeRefs); the second arg `xs: Array<int>` binds T to int via §14.5
// phase 1's constructed-type unification. The first rework (3f51d766)
// ACCEPTED this and the binder silently coerced `"s"` against int —
// measured: `v=4435280`. The rework-redo's heterogeneous-literal
// classifier rejects this with the §14.5 + §15.4 diagnostic.
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           heterogeneous_first_arg_with_binding_source_rejected_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895D1; "
        "class P "
        "  public static method F<T>(a: Array<T>, b: Array<T>): () "
        "    Console.WriteLine($\"v={a.Length}\"); "
        "  end F; "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var result := F([\"s\", 1], xs); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// D2: heterogeneous literal at 2nd position with binding source at 1st.
// `F<T>(a: Array<T>, b: Array<T>): ()` called as `F(xs, ["s", 1])` with
// `xs: Array<int>`. The 2nd arg is heterogeneous; the 1st arg binds T.
// Same reject as D1.
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           heterogeneous_second_arg_with_binding_source_rejected_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895D2; "
        "class P "
        "  public static method F<T>(a: Array<T>, b: Array<T>): () "
        "    Console.WriteLine($\"v={b.Length}\"); "
        "  end F; "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var result := F(xs, [\"s\", 1]); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// D3: nested Array<Array<T>> heterogeneous.
// `F<T>(a: Array<T>, b: Array<T>): ()` called as `F([[1], ["a"]], xs)`
// with `xs: Array<Array<int>>`. The outer literal's elements are inner
// arrays of different element types (int and string), so the outer
// literal is heterogeneous by `sameTypeRef` (inner arrays differ in
// element type). The binder would coerce the second inner array against
// Array<int> — silent. Reject.
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           nested_heterogeneous_literal_with_binding_source_rejected_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895D3; "
        "class P "
        "  public static method F<T>(a: Array<T>, b: Array<T>): () "
        "    Console.WriteLine($\"v={a.Length}\"); "
        "  end F; "
        "  public static method Main(): int "
        "    var xs: Array<Array<int>> := [[1, 2], [3, 4]]; "
        "    var result := F([[1], [\"a\"]], xs); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ===========================================================================
// REJECT — the original #4895 tripwire (heterogeneous-only source).
// Both pre-fix (335e3849) and post-rework-redo REJECT this; the
// original predicate's `!tBoundByArg` arm catches it.
// ===========================================================================

// T3 (original #4895 repro, control — unchanged): `Show<T>(xs: Array<T>)`
// called with a heterogeneous literal as the ONLY source for T. The
// literal `["s", 7]` is heterogeneous; no other argument binds T.
// Original predicate already rejects via the `!tBoundByArg` arm. The
// rework-redo still rejects (same arm + the heterogeneous classifier).
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           show_heterogeneous_literal_only_source_rejected_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895T3; "
        "static class Box "
        "  public static method Show<T>(xs: Array<T>): () "
        "    var x := xs; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C; "
        "class U "
        "  public method Bug() "
        "    Console.WriteLine(Box.Show([\"s\", 7])); "
        "  end Bug; "
        "end U;");
    BEZEL_EXPECT_TRUE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ===========================================================================
// ACCEPT — the first rework's recovered cases (narrowing preserved).
// Pre-335e3849 over-broad predicate REJECTED both; the rework-redo
// ACCEPTs both via the narrowing arm (no heterogeneous literal, so the
// rework-redo's classifier does not fire).
// ===========================================================================

// T1 (issue repro A): `Pick<T>(xs: Array<T>, x: T)` called as
// `Pick([], 7)`. The non-literal argument `7` binds T to `int` per
// §14.5 phase 1; the empty literal `[]` at `xs: Array<T>` does NOT bind
// T (no element type is determinable) but T is already bound, so the
// call is sound. The empty literal is NOT heterogeneous (no element to
// coerce). Post-rework-redo accepts and prints "7".
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           pick_empty_array_with_int_scalar_accepted_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895T1; "
        "class C "
        "  public static method Pick<T>(xs: Array<T>, x: T): T "
        "    return x; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var r := Pick([], 7); "
        "    Console.WriteLine($\"{r}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// T2 (issue repro B): `Count<T>(a: Array<T>, b: Array<T>)` called as
// `Count(xs, [])` with `xs: Array<int>`. The non-literal argument `xs`
// (typed `Array<int>`) binds T to `int` via §14.5 phase 1's
// constructed-type unification; the empty literal `[]` at
// `b: Array<T>` does NOT bind T, but T is already bound by `xs`. The
// empty literal is NOT heterogeneous. Post-rework-redo accepts and
// prints "3".
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           count_array_with_empty_second_accepted_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895T2; "
        "class C "
        "  public static method Count<T>(a: Array<T>, b: Array<T>): int "
        "    return a.Length + b.Length; "
        "  end Count; "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    Console.WriteLine($\"{Count(xs, [])}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// ===========================================================================
// ACCEPT controls — must NOT regress under the rework-redo.
// ===========================================================================

// T5 (homogeneous literal + binding source — control): `F<T>(a: Array<T>,
// b: Array<T>): ()` called as `F([1, 2, 3], xs)` with `xs: Array<int>`.
// The first arg is a homogeneous literal (all int) — it BINDS T to int.
// No heterogeneous literal; both arms of the predicate are silent. Accept.
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           homogeneous_first_arg_with_binding_source_accepted_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895T5; "
        "class P "
        "  public static method F<T>(a: Array<T>, b: Array<T>): () "
        "    Console.WriteLine($\"v={a.Length}\"); "
        "  end F; "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var result := F([1, 2, 3], xs); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v=3\n"));
}

// T6 (homogeneous literal only, no other binding source — should not
// reject): `Show<T>(xs: Array<T>)` called as `Show([1, 2, 3])`. The
// literal is homogeneous, binds T to int. The narrowing arm does not
// fire (tBoundByArg=true); the heterogeneous arm does not fire (literal
// is homogeneous). Accept.
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           show_homogeneous_literal_only_source_accepted_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895T6; "
        "static class Box "
        "  public static method Show<T>(xs: Array<T>): () "
        "    var x := xs; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Box.Show([1, 2, 3])); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// T7 (explicit `<T>`): `Show<int>([1, 2])`. The explicit type argument
// supplies T; the literal never needs to bind anything. The outer
// `!hadExplicitTypeArgs` guard skips the entire predicate. Accept.
BEZEL_TEST(heterogeneous_collection_literal_with_other_t_source_4895_rework_redo,
           explicit_type_argument_unaffected_spec_14_5_15_4) {
    auto r = runAot(
        "module M4895T7; "
        "static class Box "
        "  public static method Show<T>(xs: Array<T>): () "
        "    var x := xs; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Box.Show<int>([1, 2])); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(has4895ReworkRedoDiag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}