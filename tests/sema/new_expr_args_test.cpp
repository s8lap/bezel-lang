// Spec-derived tests for Sema slice T7 — `new T(args)` checked against
// T's sole constructor: argument count (§22.5/§22.7) and argument
// types (§5.1/§12). Overloaded constructors are skipped.

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

BEZEL_TEST(sema_new_args, matching_args_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class T public constructor(a: int, b: int) end constructor; end T; "
        "class C public method M() _ := new T(1, 2); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, too_few_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class T public constructor(a: int, b: int) end constructor; end T; "
        "class C public method M() _ := new T(1); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, too_many_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class T public constructor(a: int) end constructor; end T; "
        "class C public method M() _ := new T(1, 2); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, wrong_arg_type_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class T public constructor(a: int) end constructor; end T; "
        "class C public method M(flag: bool) _ := new T(flag); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, default_ctor_no_args_ok) {
    auto r = parseSemaAll(
        "module M; class T end T; "
        "class C public method M() _ := new T(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, default_ctor_with_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class T end T; "
        "class C public method M() _ := new T(1); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, overloaded_ctors_not_checked) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  public constructor(a: int) end constructor; "
        "  public constructor(a: int, b: int) end constructor; "
        "end T; "
        "class C public method M() _ := new T(1); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, primary_constructor_not_checked) {
    // A class with a primary constructor carries its params on the
    // TypeDecl, not as a ConstructorSymbol; T7 skips it (deferred), so
    // `new T(args)` is not flagged.
    auto r = parseSemaAll(
        "module M; class T(a: int) end T; "
        "class C public method M() _ := new T(5); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_new_args, optional_ctor_param_omitted_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class T public constructor(a: int, b: int := 0) end constructor; "
        "end T; "
        "class C public method M() _ := new T(1); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
