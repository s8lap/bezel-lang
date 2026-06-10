// Spec-derived tests for §15.4 / §22.7 — fix #599: a collection literal
// `[..]` passed as an argument to `new Array<T>(...)` (or any builtin
// collection constructor) must be a compile error.
//
// §15.4: "A collection literal has no standalone type. It is well-typed
// only in a context that provides the target type, either via type
// annotation or via the parameter type of a method call."
//
// `Array<T>` declares only `constructor()` and `constructor(capacity:
// int)` (stdlib §4.4).  Neither takes a collection type, so a collection
// literal cannot provide a valid target type in `new Array<T>([...])`.
// Before this fix the compiler silently discarded the literal and produced
// an empty array — a silent miscompile.

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

// §15.4 / §22.7 (#599 must-fail): `new Array<int>([10, 20, 30])` — the
// constructor's first (and only) parameter is `int` (capacity); the
// collection literal `[10, 20, 30]` has type `Array<int>`, not `int`.
// This must be a compile error.
BEZEL_TEST(sema_new_expr_collection_literal_599,
           array_int_ctor_collection_literal_rejected_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  using var arr := new Array<int>([10, 20, 30]); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4 / §22.7 (#599 must-fail): `new Array<string>(["a", "b"])` — same
// issue with a string element type.
BEZEL_TEST(sema_new_expr_collection_literal_599,
           array_string_ctor_collection_literal_rejected_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  using var arr := new Array<string>([\"a\", \"b\"]); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §15.4 (#599 must-not-regress): `var a: Array<int> := [10, 20, 30]` —
// typed-target declaration; the literal targets `Array<int>` directly.
// This is the CORRECT form (§15.4) and must NOT produce a diagnostic.
BEZEL_TEST(sema_new_expr_collection_literal_599,
           typed_target_array_literal_ok_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  var a: Array<int> := [10, 20, 30]; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.7 (#599 must-not-regress): `new Array<int>(8)` — valid capacity
// constructor; an `int` literal is a well-typed `int` argument.
// Must NOT produce a diagnostic.
BEZEL_TEST(sema_new_expr_collection_literal_599,
           array_int_capacity_ctor_ok_spec_22_7) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  using var arr := new Array<int>(8); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.7 (#599 must-not-regress): `new Array<int>()` — default (no-arg)
// constructor; must NOT produce a diagnostic.
BEZEL_TEST(sema_new_expr_collection_literal_599,
           array_int_default_ctor_ok_spec_22_7) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  using var arr := new Array<int>(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.4 (#599 must-fail): a Set<int> ctor also takes an optional int
// capacity, not a collection literal.
BEZEL_TEST(sema_new_expr_collection_literal_599,
           set_int_ctor_collection_literal_rejected_spec_15_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static method M() "
        "  using var s := new Set<int>([1, 2, 3]); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
