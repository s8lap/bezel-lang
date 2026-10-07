// Spec-derived regression test for issue #5447 (§2.7, v0.315):
// "static method body names instance member: silent wrong code".
//
// spec §2.7 (v0.315), *Through nesting*: "A bare reference to an enclosing
// type's **instance** member is a compile error naming this rule, for the
// reason a static body cannot make one: the nested type's body has no instance
// of the enclosing type."
//
// The fix extends lookupName's static-context filter to also reject instance
// methods and properties (not just fields), and extends
// isStaticCtxInstanceMemberOfEnclosingType to name §2.7 for those cases.
//
// Without the fix, the program compiles with a #2434 warning and exits 0 (wrong
// value), not the correct rejection.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §2.7 (v0.315): the static-context rejection for a bare instance member of
// the enclosing type, by name. Asserted on BOTH the rule citation and the
// reason, so a regression to the generic "unresolved identifier" fails here.
bool hasStaticCtxInstanceMemberDiagnostic(const JitResult& r,
                                          const char* name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(name) == std::string::npos) continue;
        if (d.message.find("instance member of the enclosing type") ==
            std::string::npos)
            continue;
        if (d.message.find("\xc2\xa7""2.7") == std::string::npos) continue;
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

// #5447 / §2.7 (v0.315): a static method body calls an instance method
// `Bump` of the enclosing class. This is a compile error naming §2.7.
BEZEL_TEST(static_method_body_instance_member_5447,
           static_method_body_calls_instance_method_rejected_names_2_7) {
    JitResult r = runAot(
        "module M; "
        "class W "
        "  public _n: int := 8; "
        "  public static method Main(): int "
        "    var x := Bump(); "
        "    return x; "
        "  end Main; "
        "  public method Bump(): int "
        "    _n := _n + 1; "
        "    return _n; "
        "  end Bump; "
        "end W;");
    if (!hasStaticCtxInstanceMemberDiagnostic(r, "Bump")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasStaticCtxInstanceMemberDiagnostic(r, "Bump"));
    // Must NOT run — the program is rejected at compile time.
    BEZEL_EXPECT_FALSE(r.ran);
}

// #5447 / §2.7 (v0.315): a static method body reads an instance property
// `N` of the enclosing class. This is a compile error naming §2.7.
BEZEL_TEST(static_method_body_instance_member_5447,
           static_method_body_reads_instance_property_rejected_names_2_7) {
    JitResult r = runAot(
        "module M; "
        "class W "
        "  public _n: int := 8; "
        "  public property N: int get => _n; end; "
        "  public static method Main(): int "
        "    var x := N; "
        "    return x; "
        "  end Main; "
        "end W;");
    if (!hasStaticCtxInstanceMemberDiagnostic(r, "N")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasStaticCtxInstanceMemberDiagnostic(r, "N"));
    // Must NOT run — the program is rejected at compile time.
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: the same class, calling the instance method through an instance.
// The instance method correctly modifies and returns 9 (8 + 1).
BEZEL_TEST(static_method_body_instance_member_5447,
           instance_method_still_accessible_through_instance_correctly) {
    JitResult r = runAot(
        "module M; "
        "class W "
        "  public _n: int := 8; "
        "  public method Bump(): int "
        "    _n := _n + 1; "
        "    return _n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var w := new W(); "
        "    return w.Bump(); "
        "  end Main; "
        "end W;");
    // Must run successfully with exit code 9.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// Control: a static method body calling a STATIC method. This MUST still
// compile and return the correct value (42). The fix must not break this.
BEZEL_TEST(static_method_body_instance_member_5447,
           static_method_body_calls_static_method_correctly) {
    JitResult r = runAot(
        "module M; "
        "class W "
        "  public static method GetValue(): int => 42; "
        "  public static method Main(): int "
        "    return GetValue(); "
        "  end Main; "
        "end W;");
    // Must run successfully with exit code 42.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Control: a static method body reading a STATIC property. This MUST still
// compile and return the correct value (7). The fix must not break this.
BEZEL_TEST(static_method_body_instance_member_5447,
           static_method_body_reads_static_property_correctly) {
    JitResult r = runAot(
        "module M; "
        "class W "
        "  public static property V: int get => 7; end; "
        "  public static method Main(): int "
        "    return V; "
        "  end Main; "
        "end W;");
    // Must run successfully with exit code 7.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// Control: an instance method body calling an instance method through `this.`
// This MUST still compile and return the correct value.
BEZEL_TEST(static_method_body_instance_member_5447,
           instance_method_body_calls_instance_method_correctly) {
    JitResult r = runAot(
        "module M; "
        "class W "
        "  public _n: int := 8; "
        "  public method Bump(): int "
        "    _n := _n + 1; "
        "    return _n; "
        "  end Bump; "
        "  public method Get(): int "
        "    return Bump(); "
        "  end Get; "
        "  public static method Main(): int "
        "    var w := new W(); "
        "    return w.Get(); "
        "  end Main; "
        "end W;");
    // Must run successfully with exit code 9.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// #5462 / §2.7 (v0.315): inherited instance field — a static method body of
// Sub : Base bare-references an instance field declared on Base.
// This case is rejected by fix #5404 (ab6405e), which predates #5447 (8f538ce).
// Fix #5404's field rejection covers inherited fields via the same lookupMember
// transitive walk. Row 8 (not this one) pins #5447. This row guards #5404's
// inherited-field path against regression.
// Expected: REJECTED with the §2.7 diagnostic.
BEZEL_TEST(static_method_body_instance_member_5447,
           inherited_instance_field_from_static_body_rejected_documents_5404) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  public instanceField: int := 99; "
        "end Base; "
        "class Sub : Base "
        "  public static method Main(): int "
        "    var x := instanceField; "
        "    return x; "
        "  end Main; "
        "end Sub;");
    if (!hasStaticCtxInstanceMemberDiagnostic(r, "instanceField")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasStaticCtxInstanceMemberDiagnostic(r, "instanceField"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// #5462 / §2.7 (v0.315): inherited instance method — a static method body of
// Sub : Base bare-references an instance method declared on Base.
// This is the row that PINs fix #5447 (8f538ce): the method rejection was
// added by #5447, not by any prior fix. Without #5447, this test fails.
// Expected: REJECTED with the §2.7 diagnostic.
BEZEL_TEST(static_method_body_instance_member_5447,
           inherited_instance_method_from_static_body_rejected_pins_5447) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  public method GetVal(): int => 77; "
        "end Base; "
        "class Sub : Base "
        "  public static method Main(): int "
        "    var x := GetVal(); "
        "    return x; "
        "  end Main; "
        "end Sub;");
    if (!hasStaticCtxInstanceMemberDiagnostic(r, "GetVal")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasStaticCtxInstanceMemberDiagnostic(r, "GetVal"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: #5462 / §2.7 (v0.315): inherited STATIC member — a static method
// body of Sub : Base bare-references a static member declared on Base.
// This is a control for #5447: static members are not affected by the fix.
// It documents that inherited static members are correctly accessible (not
// affected by #5404 either, since that covers instance fields only).
// This MUST still compile and return the correct value (55).
BEZEL_TEST(static_method_body_instance_member_5447,
           inherited_static_member_from_static_body_accepted_control) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  public static staticField: int := 55; "
        "end Base; "
        "class Sub : Base "
        "  public static method Main(): int "
        "    var x := staticField; "
        "    return x; "
        "  end Main; "
        "end Sub;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

