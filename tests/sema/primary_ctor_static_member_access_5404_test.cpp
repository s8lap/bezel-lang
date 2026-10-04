// Spec-derived regression test for issue #5404 (§22.10, v0.315):
// "static member body references primary-constructor parameter: silent zero".
//
// spec §22.10 (v0.315) "Primary constructors": a primary-constructor
// parameter is in scope for "field initializers, the inheritance-clause
// base call, and the body of any method, property, constructor, or
// destructor of the class". A `static` member is none of those (it has
// no `this`, so there is no capture field to bind). A reference there
// should be a compile error, not a silently-degraded zero.
//
// WHAT THE FIX DOES. ExprTypecheck's `lookupName` has a fallback that
// searches the enclosing type's member list via `lookupMember(currentType_, name)`.
// When `inStaticMember_` is set (we're inside a static member body),
// this fallback now filters out parameter capture fields
// (FieldSymbol with `posParam != nullptr`), so the bare name becomes
// "unresolved identifier" rather than silently lowering to zero.
//
// FAIL-WITHOUT / PASS-WITH. Without the fix, the row below compiles CLEAN
// (no error at any severity) and exits 0 (the wrong value, not 7).
// With the fix, it reports "unresolved identifier `count`".

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasUnresolvedIdentifierDiagnostic(const JitResult& r, const char* name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("unresolved identifier") == std::string::npos) continue;
        if (d.message.find(name) == std::string::npos) continue;
        return true;
    }
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s] %s\n",
                     d.severity == bezel::Severity::Error ? "E" : "W",
                     d.message.c_str());
    if (!r.stdoutText.empty())
        std::fprintf(stderr, "  stdout: %s\n", r.stdoutText.c_str());
}

}  // namespace

// #5404 / §22.10 (v0.315): a static method body references the primary-
// constructor parameter `count`. This should be a compile error
// ("unresolved identifier"), not a silent zero.
BEZEL_TEST(primary_ctor_static_member_access_5404,
           static_method_body_references_param_capture_rejected_22_10) {
    JitResult r = runAot(
        "module M; "
        "class C(count: int) "
        "  public method Get(): int => count; "
        "  public static method S(): int => count; "
        "end C; "
        "class P "
        "  public static method Main(): int "
        "    return C.S(); "
        "  end Main; "
        "end P;");
    if (!hasUnresolvedIdentifierDiagnostic(r, "count")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasUnresolvedIdentifierDiagnostic(r, "count"));
    // Must NOT run — the program is rejected at compile time.
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control row: the same class, the same `Get` instance method, and an
// instance creation. The instance method body correctly accesses the
// capture field and returns the parameterized value (7).
BEZEL_TEST(primary_ctor_static_member_access_5404,
           instance_method_body_still_accesses_param_capture_correctly_22_10) {
    JitResult r = runAot(
        "module M; "
        "class C(count: int) "
        "  public method Get(): int => count; "
        "end C; "
        "class P "
        "  public static method Main(): int "
        "    var c := new C(7); "
        "    return c.Get(); "
        "  end Main; "
        "end P;");
    // Must run successfully with exit code 7.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
