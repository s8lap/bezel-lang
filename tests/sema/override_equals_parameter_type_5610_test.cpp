// Regression tests for issue #5633 — test gap for fix #5610.
//
// b530bf05 narrowed `isObjectVirtualReplacementMember`'s `Equals` arm in
// `src/sema/TypeResolution_Checks.cpp` from bare arity-1 to a parameter-type
// test, so an `override` member on an enum or choice now requires the
// parameter to be the enum/choice's own type or nullable `Object`.
//
// Spec §8.1 declares `Object`'s own member as:
//   public virtual method Equals(other: Object?): bool;
// The non-nullable spelling `Equals(Object)` is NOT the Object slot — it is an
// overload with different semantics — and is not admitted as an override target
// on value kinds (§10.5 / §35.2).
//
// The three shapes, measured pre-fix → post-fix:
//
//   shape                              | pre-fix | post-fix
//   -----------------------------------|---------|---------
//   override Equals(other: Object)      | accept  | REJECT
//   override Equals(other: Object?)    | accept  | accept
//   override Equals(other: Color/E)    | accept  | accept
//
// The first row has no prior regression test.  The two ACCEPT rows are
// controls proving the fix is narrow rather than "reject everything arity-1".
// All three rows exist for both enum (§10.5) and choice (§35.2).
//
// GetHashCode is co-declared in every row because spec §8.3 requires it
// alongside any override Equals.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Compile-time rejection pinned by specific diagnostic text (never by `!r.ran`
// alone — a runtime crash also synthesizes a diagnostic).
bool rejectedWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

int runOk(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Builds a minimal module wrapping an enum or choice declaration followed by
// a class with a Main that runs the given body.
std::string program(const std::string& typeDecl, const std::string& body) {
    return "module M; " + typeDecl +
           "class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ═══════════════════════ Enum (§10.5) ═══════════════════════════════════════

// REJECT — pre-fix: accepted (exit 0); post-fix: rejected.
// `override Equals(Object)` (non-nullable Object parameter) is not the
// Object slot; spec §8.1 names only `Equals(Object?)`.
BEZEL_TEST(override_equals_parameter_type_5610,
           enum_override_equals_non_nullable_object_rejected_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public override method Equals(other: Object): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "may not be virtual, abstract, or override"));
}

// ACCEPT — `override Equals(Object?)` IS the Object slot (spec §8.1).
BEZEL_TEST(override_equals_parameter_type_5610,
           enum_override_equals_nullable_object_accepted_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public override method Equals(other: Object?): bool => other <> null; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        // Just verify it compiles and runs; the override is accepted.
        "return 42; ");
    BEZEL_EXPECT_EQ(runOk(p), 42);
}

// ACCEPT — `override Equals(Color)` (own-type parameter) is §10.5's admitted
// `Equals(E)` replacement.
BEZEL_TEST(override_equals_parameter_type_5610,
           enum_override_equals_own_type_accepted_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public override method Equals(other: Color): bool => this = other; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "var c := Color.Red; "
        "return (if c.Equals(Color.Red) then 42 else 1); ");
    BEZEL_EXPECT_EQ(runOk(p), 42);
}

// ═══════════════════════ Choice (§35.2) ════════════════════════════════════

// REJECT — pre-fix: accepted (exit 0); post-fix: rejected.
// Same logic as the enum case: non-nullable Object is not the Object slot.
BEZEL_TEST(override_equals_parameter_type_5610,
           choice_override_equals_non_nullable_object_rejected_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public override method Equals(other: Object): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "may not be virtual, abstract, or override"));
}

// ACCEPT — `override Equals(Object?)` IS the Object slot (spec §8.1).
BEZEL_TEST(override_equals_parameter_type_5610,
           choice_override_equals_nullable_object_accepted_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public override method Equals(other: Object?): bool => other <> null; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        // Just verify it compiles and runs; the override is accepted.
        "return 42; ");
    BEZEL_EXPECT_EQ(runOk(p), 42);
}

// ACCEPT — `override Equals(Color)` (own-type parameter) is §35.2's admitted
// `Equals(E)` replacement, the same rule §35.2 inherits from §10.5.
BEZEL_TEST(override_equals_parameter_type_5610,
           choice_override_equals_own_type_accepted_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public override method Equals(other: Color): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        // Just verify it compiles and runs; the override is accepted.
        "return 42; ");
    BEZEL_EXPECT_EQ(runOk(p), 42);
}
