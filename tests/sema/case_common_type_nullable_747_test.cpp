// fix #747 R42 / §7 / §19.5 / §20: the common type (§20) of a `case`
// expression's arms must respect nullability. An untyped `var n` binding over a
// nullable VALUE scrutinee `T?` binds the WHOLE value, including null (§19.5),
// so its arm body has type `T?`. Combined with a non-nullable arm (`null => 0`
// → `int`), the §20 common type is the NULLABLE one (`int?`): a non-nullable
// `int` widens into `int?`, but `int?` does NOT implicitly narrow to `int`.
//
// Before this fix `commonTypeOf` ignored the `nullable` flag, so
// `commonTypeOf(int, int?)` returned the NON-nullable `int`. A `case` whose
// untyped `var n` arm produced the `{i8,T}` nullable struct was then mis-typed
// `int`, and codegen built a result PHI mixing the struct with a bare `int` —
// aborting the COMPILER with a `PHINode::setIncomingValue` type-mismatch
// assertion (SIGABRT). With the common type correctly resolved to `int?`,
// returning it from an `int`-returning method is a clean §7 diagnostic, and the
// compiler never crashes.

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

bool hasErrorContaining(const bezel::test::SemaResult& r, const char* substr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(substr) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// §19.5 / §20: untyped `var n` over an `int?` scrutinee binds the whole value;
// the case's common type is `int?`. Returning it from an `int`-returning method
// is a §7 nullable-into-non-nullable error — NOT a compiler crash.
BEZEL_TEST(sema_case_common_type_nullable_747, untyped_var_binding_is_nullable_spec_19_5_20) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 1; "
        "    return case x of "
        "      null => 0, "
        "      var n => n "
        "    end; "
        "  end Main; "
        "end C;");
    // The result type is int?, so returning it from `int` is rejected (§7).
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "nullable"));
}

// CONTROL §19.5: a TYPED binding `var n: int` over the `int?` scrutinee matches
// only non-null and binds the UNWRAPPED `int`, so the arms share the common
// type `int`; returning it from an `int` method is well-typed (no §7 error).
BEZEL_TEST(sema_case_common_type_nullable_747, typed_var_binding_unwraps_no_error_spec_19_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return case x of "
        "      null => 0, "
        "      var n: int => n "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "nullable"));
}

// §20: a non-nullable `int` arm and an `int?` arm share the common type `int?`,
// so assigning the case result to an `int?` target is well-typed (no §7 error).
BEZEL_TEST(sema_case_common_type_nullable_747, common_type_widens_to_nullable_spec_20) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var x: int? := 5; "
        "    var r: int? := case x of "
        "      null => 0, "
        "      var n => n "
        "    end; "
        "    return 0 if r = null else r!; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "nullable"));
}
