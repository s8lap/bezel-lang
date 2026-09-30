// Spec-derived tests for §14.1 / §22.12 (v0.311, #5164) / #5185:
// inside a generic type's own declaration — member bodies, field
// initializers, member signatures — the bare name fixes arity 0 as
// everywhere (C.1). A bare `Counter.n` (writing the generic type's own
// name as a static-member receiver, without type arguments) is ill-formed
// inside the class body exactly as it is outside.
//
// The exception previously at ExprTypecheck_Calls.cpp:22318 (`ts ==
// currentType_`) had no spec citation; the v0.311 spec clears the
// ambiguity. The current instantiation is reached through §2.7's
// enclosing-type member lookup (bare member name → enclosing type's own
// member), NOT by writing the enclosing type's bare name as a receiver.
//
// Sema path: ExprTypecheck_Calls.cpp resolveMemberAccess, TypeSymbol
// branch (the no-exprType path where the target identifier resolves to
// a TypeSymbol directly rather than going through an instantiated
// TypeRef).

#include "support/da_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::DAResult parseSemaTc(std::string_view src) {
    return bezel::test::parseAndCheckDA(src);
}

// True iff some emitted diagnostic matches the §22.12 unqualified-generic
// static access error.
bool hasGenericStaticError(const bezel::test::DAResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("requires type arguments") != std::string::npos &&
            d.message.find("22.12") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// §14.1 / §22.12 / #5185: inside a generic type's OWN declaration, the bare
// name as a static-member receiver is ill-formed.
// ---------------------------------------------------------------------------

// (1) §14.1 / §22.12 / #5185: `Counter.n` (bare) inside `class Counter<T>`
// body is now an arity error — the same diagnostic that fires from outside
// the body.
BEZEL_TEST(sema_bare_generic_name_in_own_decl_5185,
           bare_counter_n_inside_counter_t_body_diagnoses_spec_14_1) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "  public static method Bump(): int "
        "    Counter.n := Counter.n + 1; "
        "    return Counter.n; "
        "  end Bump; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(hasGenericStaticError(r));
}

// (2) §14.1 / §22.12 / #5185: the current instantiation is reached with
// explicit type args: `Counter<T>.n` inside `class Counter<T>` body is
// well-formed.
BEZEL_TEST(sema_bare_generic_name_in_own_decl_5185,
           counter_t_n_inside_counter_t_body_ok_spec_14_1) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "  public static method Bump(): int "
        "    Counter<T>.n := Counter<T>.n + 1; "
        "    return Counter<T>.n; "
        "  end Bump; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(hasGenericStaticError(r));
}

// (3) §14.1 / §22.12 / #5185: a bare member NAME (no enclosing-type
// prefix) inside a generic class's own body uses §2.7's enclosing-type
// member lookup, NOT the static-member-access path. `n` inside Counter<T>
// resolves directly to Counter<T>'s own static field — no diagnostic.
BEZEL_TEST(sema_bare_generic_name_in_own_decl_5185,
           bare_member_name_inside_generic_class_body_ok_spec_14_1) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "  public static method Bump(): int "
        "    n := n + 1; "
        "    return n; "
        "  end Bump; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(hasGenericStaticError(r));
}