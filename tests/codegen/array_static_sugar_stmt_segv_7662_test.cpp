// Regression for #7662 — the static-call sugar `Array<int>.Sort(a)` /
// `Array.Sort(a); Array.Reverse(a);` used as an expression STATEMENT crashed
// bezelc (SIGSEGV, exit 139), flakily.
//
// Root cause: sema's #3345 static-sugar arm (ExprTypecheck_Calls.cpp) typed the
// call as Unit by passing the address of a STACK-LOCAL `TypeRef unitTy` to
// `setExprType`, which stores the pointer. The type dangled after the arm
// returned, and the codegen ExprStmt arm
// (`freeUnboundOwnedBorrowingArrayTemp` -> `isFreshOwnerlessDictMaterializerExpr`
// -> `builtinTypeName`) read `rt->astNode` through it. Fix: allocate the Unit
// TypeRef in the context arena (`ctx_.makeTypeRef()`).
//
// per stdlib §4.4 (Array<T>.Sort / Reverse), spec §7.4 (the sugar is the
// instance call on its first argument), v0.350.
//
// The crash is garbage-dependent, so the shapes that matter are also compiled
// in a loop (each iteration is a fresh fork-isolated compile) and every
// iteration's exit code and value is checked.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// g01 from the issue: [3,1,2] -> Sort -> [1,2,3]; a[0] == 1.
BEZEL_TEST(array_static_sugar_stmt_segv_7662, generic_instantiation_sort_stmt) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; "
        "  Array<int>.Sort(a); "
        "  return a[0]; "
        "end Main; end C;"), 1);
}

// g02 from the issue: Sort then Reverse -> [3,2,1]; a[0] == 3.
BEZEL_TEST(array_static_sugar_stmt_segv_7662, bare_sort_then_reverse_stmts) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; "
        "  Array.Sort(a); "
        "  Array.Reverse(a); "
        "  return a[0]; "
        "end Main; end C;"), 3);
}

// g03 from the issue, accepted part: instance Sort followed by the static
// sugar Sort on the already-sorted array.
BEZEL_TEST(array_static_sugar_stmt_segv_7662, mixed_instance_and_static_sugar) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; "
        "  a.Sort(); "
        "  Array<int>.Sort(a); "
        "  var b := a.Sorted(); "
        "  return a[0] + b[2]; "  // 1 + 3
        "end Main; end C;"), 4);
}

// g03 from the issue, rejected part: the copy form has no static spelling
// (#3345: only the in-place members are sugar), so `Array<int>.Sorted(a)` is a
// diagnostic after the static `Sort` statement — it must not crash.
BEZEL_TEST(array_static_sugar_stmt_segv_7662, copy_form_static_rejected_after_sugar_stmt) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; "
        "  a.Sort(); "
        "  Array<int>.Sort(a); "
        "  var b := Array<int>.Sorted(a); "
        "  return a[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "'Array' has no static method or property 'Sorted'"));
}

// Array.Reverse alone, and the generic spelling of it.
BEZEL_TEST(array_static_sugar_stmt_segv_7662, reverse_stmt_both_spellings) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; "
        "  Array.Reverse(a); "          // [2,1,3]
        "  Array<int>.Reverse(a); "     // [3,1,2]
        "  Array.Reverse(a); "          // [2,1,3]
        "  return a[0] * 100 / 100 + a[2]; "  // 2 + 3
        "end Main; end C;"), 5);
}

// Generic Array<T>.Sort inside a generic method (T: IComparable<T>, stdlib §4.4):
// the static sugar is a statement whose first argument's element type is a
// type parameter.
BEZEL_TEST(array_static_sugar_stmt_segv_7662, generic_method_sort_stmt) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method SortIt<T>(a: Array<T>) where T: IComparable<T> "
        "    Array<T>.Sort(a); "
        "  end SortIt; "
        "  public static method RevIt<T>(a: Array<T>) "
        "    Array.Reverse(a); "
        "  end RevIt; "
        "  public static method Main(): int "
        "    var a: Array<int> := [3, 1, 2]; "
        "    SortIt<int>(a); "      // [1,2,3]
        "    RevIt<int>(a); "       // [3,2,1]
        "    var s: Array<string> := [\"b\", \"c\", \"a\"]; "
        "    SortIt<string>(s); "   // [a,b,c]
        "    return a[0] * 10 + s[0].Length + s.Count; "  // 30 + 1 + 3
        "  end Main; "
        "end C;"), 34);
}

// Loop the compile: the dangling read is garbage-dependent, so a single
// compile can pass by luck. 12 fresh compiles, each value-checked.
BEZEL_TEST(array_static_sugar_stmt_segv_7662, looped_compile_all_shapes) {
    for (int i = 0; i < 12; ++i) {
        BEZEL_EXPECT_EQ(run(
            "module M; class C public static method Main(): int "
            "  var a: Array<int> := [3, 1, 2]; "
            "  Array<int>.Sort(a); "
            "  Array.Reverse(a); "
            "  Array.Sort(a); "
            "  return a[0] * 10 + a[2]; "  // [1,2,3] -> 13
            "end Main; end C;"), 13);
    }
}
