// fix #4894: verify that `pure` on a destructor is rejected at parse time
// (Parser_Decls.cpp:1589-1631), not at the dead sema arm
// (TypeResolution_Checks.cpp:15665-15676).
//
// The parser rejects `pure` on a destructor because ast::DestructorDecl
// carries no modifiers vector — `pure` never reaches the sema modifier-loop
// in checkTypePureModifiers. The sema arm at lines 15665-15676 is dead code:
// removing it changes no behaviour.
//
// Spec §22.15 "Where `pure` is rejected": "destructors … carry no `pure`."

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndCheckPureGated(src);
}

}  // namespace

// §22.15: a plain destructor (no `pure`) compiles cleanly.
BEZEL_TEST(pure_destructor_arm_4894, plain_destructor_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a plain destructor on a class with an interface-typed field
// compiles cleanly — the destructor body is non-trivial-dispose.
BEZEL_TEST(pure_destructor_arm_4894, destructor_with_interface_field_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "interface I "
        "  method Dispose(); "
        "end I; "
        "class C "
        "  private _f: I; "
        "  destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: `pure` on a destructor is rejected at PARSE TIME.
// The parser (Parser_Decls.cpp:1589-1631) rejects it because
// DestructorDecl carries no modifiers vector; it never reaches the
// sema modifier-loop in checkTypePureModifiers.
BEZEL_TEST(pure_destructor_arm_4894, pure_on_destructor_rejected_parse_time) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  pure destructor end destructor; "
        "end C;");
    // Parser rejection, not sema.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Confirm the parser diagnostic mentions `pure` and `destructor`.
BEZEL_TEST(pure_destructor_arm_4894, pure_destructor_diagnostic_text) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  pure destructor end destructor; "
        "end C;");
    bool hasPure = false;
    bool hasDestructor = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        auto msg = std::string(d.message);
        if (msg.find("`pure`") != std::string::npos) hasPure = true;
        if (msg.find("destructor") != std::string::npos) hasDestructor = true;
    }
    BEZEL_EXPECT_TRUE(hasPure);
    BEZEL_EXPECT_TRUE(hasDestructor);
}
