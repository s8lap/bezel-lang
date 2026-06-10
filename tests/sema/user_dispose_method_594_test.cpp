// Spec-derived tests for issue #594 — user-declared `Dispose()` method on
// a class or record is a compile error (§28.2).
//
// §28.2 states: "User code does not implement Dispose() directly.  The
// customization points are: a destructor for cleanup logic; owned fields for
// fields that should be disposed with the containing object."
//
// A user Dispose() body is never called by the compiler's ownership machinery
// (using var / owned-field teardown / explicit Dispose call all route through
// the compiler-generated Dispose), so the body silently never runs — a trap.

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

// Returns true iff any diagnostic references the §28.2 Dispose restriction.
bool has594Error(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("Dispose()") != std::string::npos &&
            d.message.find("§28.2") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// --- Must-fail: user-declared Dispose() on a class (#594 silent trap) ---

// §28.2: "User code does not implement Dispose() directly."
BEZEL_TEST(sema_user_dispose_594, class_user_dispose_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Resource "
        "  public method Dispose() "
        "    _ := 0; "
        "  end Dispose; "
        "end Resource;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has594Error(r));
}

// Also rejected when the class has other members alongside Dispose().
BEZEL_TEST(sema_user_dispose_594, class_with_field_and_dispose_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Handle "
        "  _fd: int; "
        "  public method Dispose() "
        "    _ := 0; "
        "  end Dispose; "
        "end Handle;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has594Error(r));
}

// --- Must-compile (regression): correct idioms are accepted ---

// §28.4: a destructor (the correct customization point) is accepted.
BEZEL_TEST(sema_user_dispose_594, destructor_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Resource "
        "  destructor "
        "    _ := 0; "
        "  end destructor; "
        "end Resource;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A method named something other than Dispose is not restricted.
BEZEL_TEST(sema_user_dispose_594, other_method_name_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Close() "
        "    _ := 0; "
        "  end Close; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A class with no Dispose or destructor at all — trivially fine.
BEZEL_TEST(sema_user_dispose_594, plain_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class Counter "
        "  public method Increment() "
        "    _ := 0; "
        "  end Increment; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.2: Dispose() on a record is also rejected (records cannot have a
// destructor per §9.7, but the user might still write a Dispose() method
// hoping it runs — it won't).
BEZEL_TEST(sema_user_dispose_594, record_user_dispose_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int) "
        "  public method Dispose() "
        "    _ := 0; "
        "  end Dispose; "
        "end Point;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has594Error(r));
}

// A Dispose method with parameters is NOT the synthesised Dispose() signature
// (§28.2 only restricts the zero-parameter form) — this is an unusual but
// technically distinct method that happens to share the name.
BEZEL_TEST(sema_user_dispose_594, dispose_with_params_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Dispose(reason: string) "
        "    _ := 0; "
        "  end Dispose; "
        "end C;");
    // A method Dispose(string) does not conflict with the compiler-generated
    // Dispose() (different parameter list) — not a §28.2 violation.
    BEZEL_EXPECT_FALSE(has594Error(r));
}
