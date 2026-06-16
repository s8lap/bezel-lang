// Spec-derived sema tests for fix #746 (R41) — a generic METHOD's
// `class` / `value` / `notnull` kind constraints are enforced at the call site
// (explicit and inferred type arguments).
//
// Authority: §14.2 — constraints are evaluated per-instantiation, including
// method instantiations. #729 enforced only the `type` (interface/class bound)
// kind; the `class`/`value`/`notnull` kinds were skipped, and a primitive type
// argument (no declaration) was dropped — so `Clone<int>()` with `where T: class`
// was silently accepted, a soundness hole that SIGSEGV'd via a bad dispatch.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find("14.2") != std::string::npos)
            return true;
    return false;
}

const char* kIdClass =
    "static class F "
    "  public static method Id<T>(v: T): T where T: class return v; end Id; "
    "end F; ";

}  // namespace

// §14.2: an EXPLICIT `int` type arg violates `where T: class`.
BEZEL_TEST(method_constraint_callsite_746, explicit_int_violates_class) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIdClass +
        "class C public static method Main(): int "
        "  var r := F.Id<int>(42); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2: an INFERRED record (value type) violates `where T: class`.
BEZEL_TEST(method_constraint_callsite_746, inferred_record_violates_class) {
    auto r = parseSemaAll(
        std::string("module M; record W(x: int); ") + kIdClass +
        "class C public static method Main(): int "
        "  var w := new W(7); var r := F.Id(w); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2: a class type argument SATISFIES `where T: class` — no error.
BEZEL_TEST(method_constraint_callsite_746, class_satisfies_class) {
    auto r = parseSemaAll(
        std::string("module M; class Widget public n: int := 0; end Widget; ") +
        kIdClass +
        "class C public static method Main(): int "
        "  using var w := new Widget(); var r := F.Id(w); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2: a class type argument violates `where T: value`.
BEZEL_TEST(method_constraint_callsite_746, class_violates_value) {
    auto r = parseSemaAll(
        "module M; class Widget public n: int := 0; end Widget; "
        "static class F "
        "  public static method Id<T>(v: T): T where T: value return v; end Id; "
        "end F; "
        "class C public static method Main(): int "
        "  using var w := new Widget(); var r := F.Id(w); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2: an `int` (a value type) SATISFIES `where T: value` — no error.
BEZEL_TEST(method_constraint_callsite_746, int_satisfies_value) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method Id<T>(v: T): T where T: value return v; end Id; "
        "end F; "
        "class C public static method Main(): int "
        "  var r := F.Id<int>(42); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}
