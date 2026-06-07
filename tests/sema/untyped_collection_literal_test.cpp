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
