// Spec-derived tests for §15.4: "A collection literal has no standalone type.
// It is well-typed only in a context that provides the target type ... A
// collection literal in a position where no target type is determinable is a
// compile error."
//
// Bug #471: the compiler currently silently infers a type for an untyped
// collection literal (e.g. `var a := [1, 2, 3]`) instead of rejecting it.
// These tests verify the fix.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// §15.4: `var x := [1, 2, 3]` — no target type, must be a compile error.
// Spec example: `var x := [1, 2, 3]; // ERROR: cannot infer target type`
BEZEL_TEST(untyped_collection_literal_471,
           untyped_collection_literal_rejected_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  var a := [1, 2, 3]; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4: empty literal `[]` without target type — also a compile error.
BEZEL_TEST(untyped_collection_literal_471,
           empty_untyped_collection_literal_rejected_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  var a := []; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4: discard `_ := [1, 2, 3]` — no target type, must be a compile error.
BEZEL_TEST(untyped_collection_literal_471,
           discard_untyped_collection_literal_rejected_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  _ := [1, 2, 3]; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4: annotated `var a: [3]int := [1, 2, 3]` — has a target type, OK.
// (Note: the codegen path for Array<int> has a known separate bug #465;
//  we test with inline-array target which has a working codegen path.)
BEZEL_TEST(untyped_collection_literal_471,
           annotated_inline_array_collection_literal_ok_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  var a: [3]int := [1, 2, 3]; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.4: `return [1, 2, 3]` when the method's return type provides a target
// is OK. Spec says: "`return [1, 2, 3]` is admitted when the enclosing
// method's return type determines the target type."
// We verify this doesn't create a false positive (no diagnostic).
BEZEL_TEST(untyped_collection_literal_471,
           return_collection_literal_with_return_type_ok_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M(): [3]int "
        "  return [1, 2, 3]; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.4 (#1174): the admissible target types for a collection literal are a
// CLOSED list. An INTERFACE (IEnumerable<T>/IList<T>/ICollection<T>/IReadOnly*)
// is NOT on it, so a collection literal passed directly to an interface-typed
// parameter must be REJECTED — previously it silently miscompiled to an
// empty/garbage value. A CONCRETE collection passed to the same parameter
// stays valid (only the literal-without-a-concrete-target is rejected).

// §15.4 (#1174): a collection literal as the argument to a user method whose
// parameter is `IEnumerable<char>` is rejected.
BEZEL_TEST(collection_literal_interface_arg_1174,
           collection_literal_ienumerable_arg_rejected_1174) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method Count(xs: IEnumerable<char>): int "
        "  var n := 0; "
        "  for var c in xs do n := n + 1; end; "
        "  return n; "
        "end Count; "
        "public static method Main(): int "
        "  return Count(['x', 'y', 'z']); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4 (#1174) control: the same literal passed to an `Array<int>` parameter
// (a concrete, admissible target) is accepted.
BEZEL_TEST(collection_literal_interface_arg_1174,
           collection_literal_array_arg_ok_1174) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method Sum(xs: Array<int>): int "
        "  var n := 0; "
        "  for var x in xs do n := n + x; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  return Sum([1, 2, 3]); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.4 (#1174) control: a CONCRETE `Array<int>` passed to an
// `IEnumerable<int>` parameter is accepted — only the literal is rejected.
BEZEL_TEST(collection_literal_interface_arg_1174,
           concrete_array_to_ienumerable_arg_ok_1174) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method Sum(xs: IEnumerable<int>): int "
        "  var n := 0; "
        "  for var x in xs do n := n + x; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  return Sum(a); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.4 (#1174): a collection literal as the `IEnumerable<char>` argument to the
// builtin `string.Trim(chars)` (stdlib §9.6) is rejected.
BEZEL_TEST(collection_literal_interface_arg_1174,
           collection_literal_string_trim_arg_rejected_1174) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M(): int "
        "  var s := \"xxhixx\".Trim(['x']); "
        "  return s.Length; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4 (#1174): a collection literal as the `IEnumerable<string>` argument to
// the builtin `string.Join(sep, items)` (stdlib §3.4) is rejected.
BEZEL_TEST(collection_literal_interface_arg_1174,
           collection_literal_string_join_arg_rejected_1174) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M(): int "
        "  var s := string.Join(\"-\", [\"a\", \"b\", \"c\"]); "
        "  return s.Length; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4 (#1174) control: `string.Trim` given a CONCRETE `Array<char>` value
// (not a literal) stays valid.
BEZEL_TEST(collection_literal_interface_arg_1174,
           string_trim_concrete_array_arg_ok_1174) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M(): int "
        "  var cs: Array<char> := ['x']; "
        "  var s := \"xxhixx\".Trim(cs); "
        "  return s.Length; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
