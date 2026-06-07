// Spec-derived tests for bug #485 — wrong diagnostic when overriding a
// `sealed override` member. Per §12.4, a `sealed override` prevents further
// overriding; the error must cite §12.4 ("sealed"), NOT §12.1 ("not virtual").
//
// Tests:
//   1. Repro from #485: override of a `sealed override` → error mentioning
//      "sealed", NOT "not virtual/abstract/override".
//   2. Sealed override at a deeper level (three-class chain).
//   3. POSITIVE regression: overriding a plain `virtual` member is still OK.
//   4. POSITIVE regression: overriding an (unsealed) `override` member is OK.
//   5. NEGATIVE regression: override of a genuinely-non-virtual member still
//      emits the §12.1 "not virtual" diagnostic (that path must remain).
//   6. Sealed-class derivation (whole class) still rejects (§12.4 class rule).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Returns true if any diagnostic message contains all of the given substrings.
bool anyDiagContainsAll(const bezel::test::SemaResult& r,
                        std::initializer_list<std::string_view> needles) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool all = true;
        for (auto needle : needles) {
            if (d.message.find(std::string(needle)) == std::string::npos) {
                all = false;
                break;
            }
        }
        if (all) return true;
    }
    return false;
}

// Returns true if any error diagnostic mentions the substring.
bool anyDiagContains(const bezel::test::SemaResult& r,
                     std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(std::string(needle)) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// §12.4 — test 1: repro from bug #485.
// Overriding a `sealed override` member must be rejected with a §12.4
// "sealed" diagnostic, not a §12.1 "not virtual/abstract/override" error.
BEZEL_TEST(sema_sealed_member_485, override_sealed_override_gives_sealed_diagnostic) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  public virtual method Get(): int => 1; "
        "  destructor end destructor; "
        "end A; "
        "class B : A "
        "  public sealed override method Get(): int => 2; "
        "  destructor end destructor; "
        "end B; "
        "class C : B "
        "  public override method Get(): int => 3; "
        "  destructor end destructor; "
        "  public static method Main(): int => 0; "
        "end C;");
    // Must be an error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The error must mention "sealed" (§12.4), not the §12.1 "not virtual" path.
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "sealed"));
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "not `virtual`, `abstract`, or `override`"));
}

// §12.4 — test 2: sealed override at a deeper level (A → B → C → D).
// D attempts to override Get() which is sealed in B (and inherited via C).
BEZEL_TEST(sema_sealed_member_485, sealed_override_three_levels_deep_gives_sealed_diagnostic) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  public virtual method Get(): int => 1; "
        "  destructor end destructor; "
        "end A; "
        "class B : A "
        "  public sealed override method Get(): int => 2; "
        "  destructor end destructor; "
        "end B; "
        "class C : B "
        "  destructor end destructor; "
        "end C; "
        "class D : C "
        "  public override method Get(): int => 4; "
        "  destructor end destructor; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "sealed"));
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "not `virtual`, `abstract`, or `override`"));
}

// §12.1 regression guard — test 3: overriding a plain `virtual` member is OK.
BEZEL_TEST(sema_sealed_member_485, override_of_virtual_still_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  public virtual method Get(): int => 1; "
        "  destructor end destructor; "
        "end A; "
        "class B : A "
        "  public override method Get(): int => 2; "
        "  destructor end destructor; "
        "end B;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.1 regression guard — test 4: overriding an unsealed `override` member is OK.
BEZEL_TEST(sema_sealed_member_485, override_of_unsealed_override_still_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A "
        "  public virtual method Get(): int => 1; "
        "  destructor end destructor; "
        "end A; "
        "class B : A "
        "  public override method Get(): int => 2; "
        "  destructor end destructor; "
        "end B; "
        "class C : B "
        "  public override method Get(): int => 3; "
        "  destructor end destructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.1 regression guard — test 5: `override` targeting a genuinely non-virtual
// base member must still emit the §12.1 "not virtual/abstract/override" message
// (the original §12.1 code path must not be disturbed).
BEZEL_TEST(sema_sealed_member_485, override_of_nonvirtual_still_gives_12_1_diagnostic) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public method Speak() _ := 1; end Speak; "
        "end Base; "
        "class Derived : Base "
        "  public override method Speak() _ := 2; end Speak; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // Must mention the §12.1 "not virtual" wording, NOT "sealed".
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "not `virtual`, `abstract`, or `override`"));
    BEZEL_EXPECT_FALSE(anyDiagContains(r, "sealed"));
}

// §12.4 regression guard — test 6: deriving from a sealed class is still
// rejected (whole-class sealing, unchanged from before).
BEZEL_TEST(sema_sealed_member_485, sealed_class_derivation_still_rejected_12_4) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Base end Base; "
        "class Derived : Base end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
