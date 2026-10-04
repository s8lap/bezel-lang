// Spec-derived regression test for issue #5404 (§2.7 / §11.3, v0.315):
// "static member body reads plain instance field: silent zero".
//
// spec §2.7 (v0.217, #2097) / §11.3 (member body): a static member body has
// no `this`, so an instance member is not in scope. A bare reference to an
// enclosing type's instance member is a compile error per the static-context
// restriction (§2.7: "a static body has no `this`").
//
// The lookupName fallback searches the enclosing type's member list via
// lookupMember(currentType_, name). When inStaticMember_ is set, this fallback
// now filters out instance fields (FieldSymbol without "static" in modifiers),
// so the bare name becomes "unresolved identifier" rather than silently
// lowering to zero.
//
// FAIL-WITHOUT / PASS-WITH. Without the fix, the row below compiles CLEAN
// (no error at any severity) and exits 0 (the wrong value, not 99).
// With the fix, it reports "unresolved identifier `value`".

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

// #5404 / §2.7 / §11.3 (v0.315): a static method body reads a plain
// instance field `value`. This should be a compile error
// ("unresolved identifier"), not a silent zero.
BEZEL_TEST(static_member_instance_field_access_5404_instance_field,
           static_method_body_references_instance_field_rejected_2_7_11_3) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private value: int := 99; "
        "  public static method S(): int => value; "
        "end C; "
        "class P "
        "  public static method Main(): int "
        "    return C.S(); "
        "  end Main; "
        "end P;");
    if (!hasUnresolvedIdentifierDiagnostic(r, "value")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasUnresolvedIdentifierDiagnostic(r, "value"));
    // Must NOT run — the program is rejected at compile time.
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control row: the same class, reading the instance field through an instance
// method. The instance method correctly accesses the field and returns 99.
BEZEL_TEST(static_member_instance_field_access_5404_instance_field,
           instance_method_body_still_accesses_instance_field_correctly_2_7_11_3) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private value: int := 99; "
        "  public method Get(): int => value; "
        "end C; "
        "class P "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    return c.Get(); "
        "  end Main; "
        "end P;");
    // Must run successfully with exit code 99.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// Control row: a static method body reading a STATIC field. This MUST still
// compile and return the correct value (5). The fix must not break this.
BEZEL_TEST(static_member_instance_field_access_5404_instance_field,
           static_method_body_still_reads_static_field_correctly_2_7_11_3) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static F: int := 5; "
        "  public static method S(): int => F; "
        "end C; "
        "class P "
        "  public static method Main(): int "
        "    return C.S(); "
        "  end Main; "
        "end P;");
    // Must run successfully with exit code 5.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
