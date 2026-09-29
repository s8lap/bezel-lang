// Spec-derived regression test for issue #4465.
//
// §14.2 (constraint satisfaction checked wherever instantiation occurs):
// when a static member is accessed through a generic class instantiation at the
// call site (`Helper<BadType>.StaticMethod(...)`), the class's `where` clause
// was silently dropped. The constraint was enforced when the generic class was
// instantiated as a local/field/parameter type, but NOT when it was the receiver
// of a static member access.
//
// Root cause: checkGenericConstraintsAtUseSite (§14.2 static-call entry point,
// added in fix #3160) deferred type-bound failures to the resolveTypes-time
// Resolver's deferredTypeBoundFailures_ list. When called from ExprTC via the
// free function checkGenericConstraintsAtExpr, a temporary Resolver was created,
// checkGenericConstraintsAtUseSite populated its deferredTypeBoundFailures_, and
// the Resolver was discarded without ever flushing. At ExprTC time all types
// are fully resolved, so flushing immediately is safe — fix #4465 calls
// flushDeferredTypeBoundFailures() after checkGenericConstraintsAtUseSite.
//
// What this test pins:
//   * REJECT: Helper<NotBase>.Use(...) — class-level `where T: Base` violated,
//     static-call receiver path. Before the fix: accepted silently. After:
//     "type argument `NotBase` does not satisfy the constraint `where T: Base`".
//   * REJECT: Helper<NotBase>.Tag — static PROPERTY form, same constraint.
//   * ACCEPT-CONTROL: Helper<Widget>.Use(...) — Widget extends Base, so the
//     constraint is satisfied and the static member is accessible.
//   * ACCEPT-CONTROL: Helper<Widget>.Tag — property form, same control.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasConstraintError(const bezel::test::SemaResult& r,
                        const char* fragment) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find(fragment) != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

}  // namespace

// --- §14.2 REJECT: static method call through generic class, constraint
// violated at the receiver's class-level where clause.
//
// Issue #4465: `Helper<NotBase>.Use(...)` was accepted silently before the fix.
// NotBase does not extend Base, so `where T: Base` is violated at the
// static-call receiver instantiation site.

BEZEL_TEST(static_member_through_generic_class_4465,
           static_call_through_generic_class_rejects_constraint_violation) {
    auto r = semaAll(
        "module M; "
        "class Base public x: int; end Base; "
        "class Widget: Base end Widget; "
        "class NotBase public y: int; end NotBase; "
        "class Helper<T> where T : Base "
        "  public static method Use(t: T): int return 0; end Use; "
        "end Helper; "
        "class Program "
        "  public static method Main(): int "
        "    return Helper<NotBase>.Use(new NotBase()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r, "where T: Base"));
}

// --- §14.2 REJECT: static property access through generic class, same constraint.

BEZEL_TEST(static_member_through_generic_class_4465,
           static_property_through_generic_class_rejects_constraint_violation) {
    auto r = semaAll(
        "module M; "
        "class Base public x: int; end Base; "
        "class Widget: Base end Widget; "
        "class NotBase public y: int; end NotBase; "
        "class Helper<T> where T : Base "
        "  public static property Tag: int := 42; "
        "end Helper; "
        "class Program "
        "  public static method Main(): int "
        "    return Helper<NotBase>.Tag; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r, "where T: Base"));
}

// --- §14.2 ACCEPT-CONTROL: well-formed instantiation stays accepted.
//
// Widget extends Base, so `where T: Base` is satisfied. The static call
// must compile without constraint errors.

BEZEL_TEST(static_member_through_generic_class_4465,
           static_call_through_generic_class_accepts_valid_instantiation) {
    auto r = semaAll(
        "module M; "
        "class Base public x: int; end Base; "
        "class Widget: Base end Widget; "
        "class Helper<T> where T : Base "
        "  public static method Use(t: T): int return t.x; end Use; "
        "end Helper; "
        "class Program "
        "  public static method Main(): int "
        "    return Helper<Widget>.Use(new Widget()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!hasAnyError(r));
}

// --- §14.2 ACCEPT-CONTROL: static property form, valid instantiation.

BEZEL_TEST(static_member_through_generic_class_4465,
           static_property_through_generic_class_accepts_valid_instantiation) {
    auto r = semaAll(
        "module M; "
        "class Base public x: int; end Base; "
        "class Widget: Base end Widget; "
        "class Helper<T> where T : Base "
        "  public static property Tag: int := 42; "
        "end Helper; "
        "class Program "
        "  public static method Main(): int "
        "    return Helper<Widget>.Tag; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!hasAnyError(r));
}
