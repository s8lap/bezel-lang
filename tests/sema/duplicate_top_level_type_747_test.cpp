// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT) — two top-level types
// in one module sharing a name.
//
// Authority: §11.3 / §13.2 — §11.3 states the clash rule for members and NESTED
// types ("two ... nested types with the same name ... are a compile error").
// The module is the top-level namespace and the same well-formedness rule
// applies: two same-named top-level types are unresolvable. Bug: the second
// declaration silently shadowed the first (`Foo.Val` read the wrong type's
// static), with no diagnostic. The fix (checkDuplicateTopLevelTypes, run during
// type resolution) rejects the second declaration.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDuplicateTypeError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("already declared in this module") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §11.3: two top-level classes with the same name are rejected.
BEZEL_TEST(duplicate_top_level_type_747, two_classes_same_name_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Foo public static Val: int := 1; end Foo; "
        "class Foo public static Val: int := 2; end Foo; "
        "class P public static method Main(): int return Foo.Val; end Main; end P;");
    BEZEL_EXPECT_TRUE(hasDuplicateTypeError(r));
}

// §11.3: a class and a record sharing a name are rejected (different kinds,
// same name — still a clash).
BEZEL_TEST(duplicate_top_level_type_747, class_and_record_same_name_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Bar public static V: int := 1; end Bar; "
        "record Bar(X: int); "
        "class P public static method Main(): int return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(hasDuplicateTypeError(r));
}

// §11.3 regression guard: distinct top-level type names are accepted.
BEZEL_TEST(duplicate_top_level_type_747, distinct_names_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A public static V: int := 1; end A; "
        "class B public static V: int := 2; end B; "
        "record C(X: int); "
        "enum E Red, Green; end E; "
        "interface I method M(): int; end I; "
        "class P public static method Main(): int return A.V + B.V; end Main; end P;");
    BEZEL_EXPECT_FALSE(hasDuplicateTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
