// Spec-derived tests for §22.12 / issue #696: per-instantiation static
// observability — accessing a generic class's static member without type
// arguments is ILL-FORMED, including inside the class's own body
// (per §14.1 / #5185, v0.311).
//
// §22.12 (normative): "A static member of a *generic* class is accessed by
// supplying the type arguments on the type name in receiver position —
// `Registry<int>.Register()`, `Cache<string>.Default`. Each distinct
// instantiation has its own static state."
//
// The compiler must diagnose bare `Counter.n` when Counter<T> is generic and
// the access is from outside the class's own body.  The type-argumented form
// `Counter<int>.n` is well-formed.
//
// Sema path: ExprTypecheck_Calls.cpp resolveMemberAccess, TypeSymbol branch
// (the no-exprType path where the target identifier resolves to a TypeSymbol
// directly rather than going through an instantiated TypeRef).

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
// ILL-FORMED: bare `Counter.n` where Counter<T> is generic (§22.12)
// ---------------------------------------------------------------------------

// (1) §22.12 / #696: reading a generic static field without type args from a
// non-generic method is ill-formed — the compiler must diagnose it.
BEZEL_TEST(sema_generic_static_696,
           bare_generic_static_field_read_outside_class_diagnoses_spec_22_12) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "public class Program "
        "  public static method Main(): int "
        "    return Counter.n; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasGenericStaticError(r));
}

// (2) §22.12 / #696: writing a generic static field without type args from a
// non-generic method is ill-formed.
BEZEL_TEST(sema_generic_static_696,
           bare_generic_static_field_write_outside_class_diagnoses_spec_22_12) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "public class Program "
        "  public static method Main(): int "
        "    Counter.n := 1; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasGenericStaticError(r));
}

// (3) §22.12 / #696: two-param generic class, bare access from outside is
// ill-formed for each member.
BEZEL_TEST(sema_generic_static_696,
           two_param_generic_bare_static_read_diagnoses_spec_22_12) {
    auto r = parseSemaTc(
        "module M; "
        "public class Pair<A, B> "
        "  public static count: int := 0; "
        "end Pair; "
        "public class Program "
        "  public static method Main(): int "
        "    return Pair.count; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasGenericStaticError(r));
}

// ---------------------------------------------------------------------------
// WELL-FORMED: type-argumented access `Counter<int>.n` is valid (§22.12)
// ---------------------------------------------------------------------------

// (4) §22.12 / #696: `Counter<int>.n` — fully qualified, well-formed.
BEZEL_TEST(sema_generic_static_696,
           typed_generic_static_field_read_outside_class_ok_spec_22_12) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "public class Program "
        "  public static method Main(): int "
        "    return Counter<int>.n; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasGenericStaticError(r));
}

// (5) §22.12 / #696: `Counter<string>.n` — second distinct instantiation,
// also well-formed.
BEZEL_TEST(sema_generic_static_696,
           typed_generic_static_field_string_outside_class_ok_spec_22_12) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "public class Program "
        "  public static method Main(): int "
        "    return Counter<string>.n; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasGenericStaticError(r));
}

// ---------------------------------------------------------------------------
// ILL-FORMED: bare access INSIDE the generic class's own body is also
// diagnosed (per §14.1 / #5185, v0.311 — the bare name fixes arity 0 the
// same as outside; the current instantiation is reached through §2.7's
// enclosing-type member lookup, not by writing the bare enclosing type as
// a receiver).
// ---------------------------------------------------------------------------

// (6) §14.1 / §22.12 / #5185: bare `Counter.n` inside Counter<T>'s OWN method
// body fixes arity 0 and must produce the same diagnostic as outside the body.
BEZEL_TEST(sema_generic_static_696,
           bare_generic_static_inside_own_class_body_ok_spec_22_12) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter<T> "
        "  public static n: int := 0; "
        "  public static method Bump() "
        "    Counter.n := Counter.n + 1; "
        "  end Bump; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(hasGenericStaticError(r));
}

// (7) §14.1 / §22.12 / #5185: bare `Tag.disposed` inside Tag<T>'s destructor
// body fixes arity 0 — must be diagnosed (mirrors the #684 test shape).
BEZEL_TEST(sema_generic_static_696,
           bare_generic_static_in_destructor_body_ok_spec_22_12) {
    auto r = parseSemaTc(
        "module M; "
        "public class Tag<T> "
        "  public static disposed: int := 0; "
        "  public constructor() end constructor; "
        "  destructor "
        "    Tag.disposed := Tag.disposed + 1; "
        "  end destructor; "
        "end Tag;");
    BEZEL_EXPECT_TRUE(hasGenericStaticError(r));
}

// ---------------------------------------------------------------------------
// WELL-FORMED: non-generic class static field access is unaffected (§22.12)
// ---------------------------------------------------------------------------

// (8) §22 regression guard: a non-generic class's static field is accessible
// without type arguments — must not diagnose.
BEZEL_TEST(sema_generic_static_696,
           nongeneric_class_static_field_ok_spec_22) {
    auto r = parseSemaTc(
        "module M; "
        "public class Counter "
        "  public static n: int := 0; "
        "end Counter; "
        "public class Program "
        "  public static method Main(): int "
        "    return Counter.n; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasGenericStaticError(r));
}
