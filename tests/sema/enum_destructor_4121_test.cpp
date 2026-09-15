// Spec-derived test for fix #4121 — `Resolver::checkEnumRestrictions`
// (src/sema/TypeResolution_Checks.cpp) had no destructor check, so an enum
// body declaring a `destructor` compiled at exit 0 with zero diagnostics.
//
// §10.4 ("Methods, properties, static members", v0.298) states the
// POSITIVE, exhaustive list: "An enum may declare exactly the following
// members, and no others: instance methods, expression-bodied read-only
// instance properties, static methods, `static readonly` fields ..., `const`
// members, and operator overloads" — no destructor entry. §8.7 ("`Dispose`
// is not on `Object`", v0.298) states: "Static classes, records, enums,
// choices, primitives, and inline arrays do not implement `IDisposable`" —
// so an enum has no `Dispose()` surface for a destructor to hook. The spec's
// Appendix A preamble "Shared-production over-admissions" index (the
// unnumbered table before A.1) independently names this exact gap in its
// `Member` in an enum-or-choice body row: "... destructors by §8.7" — for
// BOTH enum and choice bodies.
//
// `checkChoiceRestrictions` already rejects the identical shape on choices
// (fix #3869): "choice `Shape` may not declare a destructor (§35.1 /
// §35.2)". Before THIS fix, enum and choice DISAGREED post-#3869 — #3869's
// own comment explicitly declined to touch the enum side. Per this issue's
// own instruction to establish which side was right before making them
// agree: §10.4's exhaustive admitted-member list plus §8.7's "no
// `IDisposable`" rule together make CHOICE's existing rejection the one the
// spec supports — this fix makes ENUM agree with CHOICE, not the reverse.
//
// MEASURED on R17 cycle-3 base `3494cd1` (pre-fix):
//   enum Color: int Red, Green, Blue; destructor end destructor; end Color;
// compiled at exit 0, zero diagnostics, while the choice analogue was
// already correctly rejected.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// REJECT — the issue's own measured repro.
// =====================================================================

BEZEL_TEST(enum_destructor_4121, enum_destructor_rejected_spec_10_4_8_7) {
    auto r = runAot(
        "module M; "
        "enum Color4121: int "
        "  Red, Green, Blue; "
        "  destructor end destructor; "
        "end Color4121; "
        "class C4121 public static method Main(): int "
        "  return 0; "
        "end Main; end C4121;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "enum `Color4121` may not declare a destructor"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§8.7"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§10.4"));
}

// REJECT — the same shape with no explicit underlying-type annotation (the
// issue's own default-underlying-type repro shape, §10.3's default `int`).
BEZEL_TEST(enum_destructor_4121,
           enum_destructor_default_underlying_rejected_spec_10_4_8_7) {
    auto r = runAot(
        "module M; "
        "enum Signal4121 "
        "  Red, Green; "
        "  destructor end destructor; "
        "end Signal4121; "
        "class C4121b public static method Main(): int "
        "  return 0; "
        "end Main; end C4121b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "enum `Signal4121` may not declare a destructor"));
}

// =====================================================================
// ACCEPT — legitimate destructor use (classes), the choice sibling's
// unaffected pre-existing rejection, and an enum with NO destructor, must
// all keep working exactly as before (LANE_RULES §15).
// =====================================================================

// ACCEPT — a destructor on a CLASS, the normal, intended case: entirely
// unaffected by an enum-only check.
BEZEL_TEST(enum_destructor_4121, class_destructor_accepted_control_spec_28_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Resource4121 "
        "  private _fd: int := 0; "
        "  destructor "
        "  end destructor; "
        "end Resource4121; "
        "class C4121c public static method Main(): int "
        "  var r := new Resource4121(); "
        "  return 42; "
        "end Main; end C4121c;"),
        42);
}

// ACCEPT — a `using var` / `IDisposable` shape over a class with a
// destructor: the compiler-generated `Dispose()` + destructor interaction
// on the ONE kind that legitimately has it must be unaffected.
BEZEL_TEST(enum_destructor_4121,
           using_disposable_class_destructor_accepted_control_spec_28_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Resource4121b "
        "  private _fd: int := 0; "
        "  destructor "
        "    _fd := -1; "
        "  end destructor; "
        "end Resource4121b; "
        "class C4121d public static method Main(): int "
        "  using var r := new Resource4121b(); "
        "  return 42; "
        "end Main; end C4121d;"),
        42);
}

// ACCEPT — an enum with NO destructor and an ordinary §10.4-admitted method
// must be entirely unaffected by adding the new DestructorSymbol arm.
BEZEL_TEST(enum_destructor_4121,
           enum_without_destructor_accepted_control_spec_10_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Color4121c: int "
        "  Red, Green, Blue; "
        "  public method IsPrimary(): bool => true; "
        "end Color4121c; "
        "class C4121e public static method Main(): int "
        "  var c := Color4121c.Red; "
        "  return (if c.IsPrimary() then 42 else 1); "
        "end Main; end C4121e;"),
        42);
}

// ACCEPT (control) — the choice destructor rejection (#3869, already
// correct) must be UNCHANGED by this enum-only fix: same message text,
// same citation.
BEZEL_TEST(enum_destructor_4121,
           choice_destructor_rejection_unchanged_control_spec_35_1_35_2) {
    auto r = runAot(
        "module M; "
        "choice Shape4121 "
        "  Circle(radius: double), Square(side: double); "
        "  destructor end destructor; "
        "end Shape4121; "
        "class C4121f public static method Main(): int "
        "  return 0; "
        "end Main; end C4121f;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "choice `Shape4121` may not declare a destructor"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§35.1 / §35.2"));
}
