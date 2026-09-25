// Spec-derived regression test for issue #4895
// (spec §14.5 phase 3 + §15.4, v0.307).
//
// THE DEFECT. A generic method's type parameter whose ONLY source is a
// collection-literal argument (and which does not appear in the return
// type) used to be bound by the binder's element-by-element
// `checkAssignable` walk: T was bound to the first element's resolved
// type, the second element's type failed the assignability check against
// that first-element type, but the call still typechecked and codegen
// proceeded with T wrong.
//
// §14.5 phase 3 requires the bound to be a single determinate type; the
// standard example is a return type or argument whose declared type
// fixes T. §15.4 (collection-literal target types) is the canonical
// statement: a `[…]` literal is well-typed only in a context that
// provides the target type — "[…] is admitted when the enclosing
// method's return type determines the target type" — and the same rule
// applies to a generic method's parameter type: if the parameter type
// is `Array<T>` and the only source of T is the literal, the literal's
// elements would have to bind T circularly. A target type outside the
// parameter slot (a return type, a typed local, an explicit `<T>`) is
// what makes it sound.
//
// MEASURED on the pre-fix compiler (origin/main @ 216b1cb88f47):
// `bezelc` on `Show(["s", new A()])` ran clean, no diagnostics,
// codegen succeeded, and the program printed `4435344|381342640` at
// runtime — T was bound to `string` (the first element's type), then
// the second element `new A()` (a `ptr`-sized reference) was
// reinterpreted through that first element's representation, giving
// the truncated pointer values in the output.
//
// The fix: in `typeCall` (ExprTypecheck_Calls.cpp), after the existing
// argument-inference loop resolves every generic parameter, walk each
// parameter position whose declared type still mentions one of the
// method's type parameters; if the actual argument at that position is
// a collection literal whose elements do not all share a single
// determined type, reject the call with a diagnostic that names the
// parameter and the §14.5 / §15.4 authority.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff some emitted diagnostic mentions the §14.5 phase-3
// collection-literal-only-source inference failure.
bool has4895Diag(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("phase 3 of §14.5") != std::string::npos &&
            d.message.find("collection literal") != std::string::npos) {
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
// REJECT tests — these FAIL without the fix, PASS with it
// ===========================================================================

// T1 (issue #4895 repro, class-typed variant): `Show(["s", new A()])`.
// The literal's elements are heterogeneous (a `string` and a class-typed
// `A`), so they do not share a single TypeRef. Without the fix T gets
// bound to the first element (`string`), the `A` is reinterpreted through
// the string representation, and the call typechecks. With the fix the
// call is rejected at compile time.
BEZEL_TEST(collection_literal_only_tparam_source_4895,
           heterogeneous_class_typed_literal_rejected_spec_14_5_15_4) {
    auto r = runAot(
        "module M; "
        "class A public constructor() end constructor; end A; "
        "static class Box "
        "  public static method Show<T>(items: Array<T>): () "
        "    var x := items; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C; "
        "class U "
        "  public method Bug() "
        "    Console.WriteLine(Box.Show([\"s\", new A()])); "
        "  end Bug; "
        "end U;");
    BEZEL_EXPECT_TRUE(has4895Diag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// T2 (heterogeneous primitive variant): `Show([true, 1])`. `bool` and
// `int` are distinct primitive TypeRefs; the literal does not determine
// a single T. Rejected by the fix.
BEZEL_TEST(collection_literal_only_tparam_source_4895,
           heterogeneous_primitive_literal_rejected_spec_14_5_15_4) {
    auto r = runAot(
        "module M; "
        "static class Box "
        "  public static method Show<T>(items: Array<T>): () "
        "    var x := items; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C; "
        "class U "
        "  public method Bug() "
        "    Console.WriteLine(Box.Show([true, 1])); "
        "  end Bug; "
        "end U;");
    BEZEL_EXPECT_TRUE(has4895Diag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ===========================================================================
// ACCEPT controls — these PASS both before AND after the fix
// (the fix is narrow: it only rejects literals whose elements do not
// share a single determined type)
// ===========================================================================

// T3 (class-typed, homogeneous): `Show([a, a])` where `a: A`. Both
// elements have the same determined TypeRef `A`, so the literal binds
// T to A. The fix leaves this accepted (and indeed the binder's
// element-by-element walk still works on it).
BEZEL_TEST(collection_literal_only_tparam_source_4895,
           homogeneous_class_typed_literal_accepted_spec_14_5_15_4) {
    auto r = runAot(
        "module M; "
        "class A public constructor() end constructor; end A; "
        "static class Box "
        "  public static method Show<T>(items: Array<T>): () "
        "    var x := items; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var a: A := new A(); "
        "    Console.WriteLine(Box.Show([a, a])); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(has4895Diag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// T4 (explicit `<int>`): `Show<int>([1, 2])`. The explicit type argument
// supplies T; the literal never needs to bind anything. The fix does not
// engage at all (the explicit-args path skips the unbound-parameter
// loop entirely), so this still compiles cleanly.
BEZEL_TEST(collection_literal_only_tparam_source_4895,
           explicit_type_argument_unaffected_spec_14_5_15_4) {
    auto r = runAot(
        "module M; "
        "static class Box "
        "  public static method Show<T>(items: Array<T>): () "
        "    var x := items; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Box.Show<int>([1, 2])); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(has4895Diag(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// T5 (no type parameter at all — non-generic method): a plain
// `Show(items: Array<string>)` whose elements happen to be heterogeneous
// strings / ints is NOT a type-parameter inference question (there is
// no T). The fix is silent on this case; the assignability check
// against the declared parameter type already rejects mismatched
// elements. We exercise only the "no T, heterogeneous elements" case to
// pin the fix's narrowness — it must not regress non-generic methods.
BEZEL_TEST(collection_literal_only_tparam_source_4895,
           non_generic_heterogeneous_literal_still_assigned_check) {
    auto r = runAot(
        "module M; "
        "static class Box "
        "  public static method Show(items: Array<string>): () "
        "    var x := items; "
        "  end Show; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C; "
        "class U "
        "  public method Bad() "
        "    Console.WriteLine(Box.Show([\"a\", \"b\"])); "
        "  end Bad; "
        "end U;");
    BEZEL_EXPECT_FALSE(has4895Diag(r));
    // The literal IS valid here (two strings, declared Array<string>):
    // no error of any kind.
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}
