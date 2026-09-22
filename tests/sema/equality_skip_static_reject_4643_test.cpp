// Spec-derived sema tests for fix #4643 — §9.2 inert-annotation rule on a
// static member, mirroring §9.8's analogous rule.
//
// Spec §9.2 line 1419, verbatim:
//   "On a computed (expression-bodied) property, a method, a static member,
//    an enum constant, or a choice case (the case is the discriminant —
//    skipping it is incoherent), `equality skip` is a compile error,
//    mirroring §9.8's inert-annotation rule: such members never participate
//    in equality, and silently ignoring the clause would be the
//    inert-looking-but-load-bearing annotation rationale §18 warns against."
//
// The other arms of the rule (computed property / method / enum constant /
// choice case) are caught at PARSE time — the parser does not admit
// `equality` at those grammar positions. Only the static-member arm needs
// sema, because a static FIELD is a fully valid declaration whose grammar
// admits the clause (A.4 FieldDecl, line 10952 — `EqualityClause?`
// appearing BEFORE the Identifier in the production). Sema is the gate.
//
// This file pins the static-field arm specifically. The diagnostic must
// carry the substring `equality skip` so the user (and the lane's grep-
// based regression oracle) can locate the failure unambiguously.
//
// Per CLAUDE.md "Never weaken a test to make it pass", the diagnostic
// substring `equality skip` is the load-bearing assertion: it pins the
// rule itself, not just "this rejected something." A future regression
// that surfaced a generic "expected ;" error instead of the rule's
// specific message would slip past a `failedAtCompileTimeWithRealLocation`
// check alone but fail this substring check — the right direction for a
// hard-coded rule to drift.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// Lane regression — `equality skip` on a STATIC FIELD is the inert-
// annotation arm of §9.2. The parser accepts (the grammar admits it on
// FieldDecl); sema rejects with a diagnostic carrying the clause
// spelling `equality skip` so a user can grep for it. Mirrors §9.8's
// own static-skip inert-annotation rule (cited at line 1419).
// =====================================================================

BEZEL_TEST(sema_equality_skip_s9_2_0020,
           class_static_readonly_field_equality_skip_rejected) {
    auto r = runAot(
        "module M; "
        "public class Config4643 "
        "  public static readonly equality skip Default: int := 0; "
        "end Config4643; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "equality skip"));
}

BEZEL_TEST(sema_equality_skip_s9_2_0020,
           record_static_readonly_field_equality_skip_rejected) {
    auto r = runAot(
        "module M; "
        "public record ConfigRec4643 "
        "  public static readonly equality skip Default: int := 0; "
        "end ConfigRec4643; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "equality skip"));
}

// =====================================================================
// A static PROPERTY carrying `equality skip` is the parallel inert-
// annotation case from §9.2 line 1419. The parser accepts (A.4
// auto-property PropertyBody admits the clause); sema rejects.
// =====================================================================

BEZEL_TEST(sema_equality_skip_s9_2_0020,
           static_auto_property_equality_skip_rejected) {
    auto r = runAot(
        "module M; "
        "public class StaticProp4643 "
        "  public static readonly property Stamp: int := 0 equality skip; "
        "end StaticProp4643; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "equality skip"));
}

// =====================================================================
// A non-static (instance) field carrying `equality skip` MUST be
// accepted — the rule's exception clause. This is the same-shape
// inverted control that pins the parser fix's other half (s9_2_0009):
// without it, a future regression that blanket-rejects every
// `equality skip` would pass. (The fail-without proven on the whole
// lane.)
// =====================================================================

BEZEL_TEST(sema_equality_skip_s9_2_0009,
           instance_field_equality_skip_compiles) {
    // The spec §9.2 worked example, line 1397:
    //   record Interval
    //     public readonly property Lo: int;
    //     public readonly property Hi: int;
    //     private equality skip _cachedLen: int;
    //     ...
    // Reduced to the field-only carve-out the lane owns: an instance
    // field with `equality skip` on a record MUST compile, and the
    // generated `=` must ignore the skipped field (the cache example:
    // two records with different `_cachedLen` are `=`).
    //
    // The test source mirrors the structure of the s9_2_0009 conformance
    // case (a body record with one `equality skip` field and one auto-
    // property, both contributing storage, with `Main` writing the
    // comparison result). The expected `True` is the lane's evidence that
    // the synthesized `=` ignored `_cachedLen` — its absence would flip
    // the output to `False`.
    auto r = runAot(
        "module M; "
        "public record Interval(Lo: int, Hi: int, _cachedLen: int equality skip); "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Interval(1, 5, 4); "
        "    var b := new Interval(1, 5, 999); "
        "    Console.WriteLine(a = b); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
    }
}