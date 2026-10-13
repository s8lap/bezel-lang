// Spec-derived tests for fix #4186 — an `invariant` member on a RECORD body
// or a `static class` body compiled with ZERO diagnostics.
//
// §22.16 ("Contract clauses", v0.298): "A method or constructor declaration
// may carry contract clauses ... and a class body may declare invariant
// members (`invariant(E);`)." A record is not a class, and a static class
// has no instance to check a predicate over.
//
// Grammar authority (Appendix A.4, `InvariantDecl` production comment):
// "class bodies only; rejected semantically in records and static classes
// (§22.16)". `ast::InvariantDecl`'s own doc comment (src/ast/Node.hpp)
// repeats this verbatim: "Class-body-only per A.4; semantically rejected in
// records, static classes, and outside class bodies." §22.16's "Class
// invariants" subsection's own "Out of scope in this revision" list makes
// the static-class half explicit: "static-class and static-state invariants
// (an `invariant` in a static class is a compile error — no instance
// boundary exists)".
//
// `invariant` clauses are not modeled as a `Symbol` subclass in
// `TypeSymbol::members` at all — `SymbolCollection` never creates one for
// `ast::InvariantDecl` — so a `t->members`-based walk cannot see one. This
// fix adds `Resolver::checkRecordAndStaticClassInvariantMembers`
// (TypeResolution_Checks.cpp), which reads the raw AST member list
// (`t->decl`'s `ast::TypeDecl::members`) directly instead — the same shape
// fix #4115's `checkEnumChoiceInvariantMembers` uses for enum/choice bodies
// (tests/sema/enum_choice_invariant_member_4115_test.cpp), which explicitly
// left records and static classes out of its own scope and named this issue
// as the follow-up.
//
// MEASURED on origin/main-derived R18 base `3999278` (pre-fix): both
//   record Point4186 public X: int; public Y: int; invariant(true); end Point4186;
// and
//   static class Utils4186 invariant(true); end Utils4186;
// compiled via `runAot` at exit 0, zero diagnostics (the issue's own repro).

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
// REJECT — §22.16 / Appendix A.4 `InvariantDecl` production comment.
// =====================================================================

// The issue's own repro, verbatim in shape.
BEZEL_TEST(record_static_class_invariant_4186,
           record_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "record Point4186 "
        "  public X: int; "
        "  public Y: int; "
        "  invariant(true); "
        "end Point4186; "
        "class C4186 public static method Main(): int "
        "  return 0; "
        "end Main; end C4186;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "record `Point4186` may not declare an `invariant` member"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.16"));
}

// The issue's own repro, verbatim in shape.
BEZEL_TEST(record_static_class_invariant_4186,
           static_class_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "static class Utils4186 "
        "  invariant(true); "
        "end Utils4186; "
        "class C4186b public static method Main(): int "
        "  return 0; "
        "end Main; end C4186b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r,
        "static class `Utils4186` may not declare an `invariant` member"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.16"));
}

// REJECT — the `static invariant` spelling. v0.338 (#5939): an invariant
// member takes no `static` qualifier, so the qualifier is rejected at the
// parser (its needle is pinned by tests/sema/static_invariant_*) while the
// body-kind rejection of this file's own kind is what this row is about.
BEZEL_TEST(record_static_class_invariant_4186,
           record_static_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "record Point4186s "
        "  public X: int; "
        "  static invariant(true); "
        "end Point4186s; "
        "class C4186s public static method Main(): int "
        "  return 0; "
        "end Main; end C4186s;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "an invariant member takes no `static` qualifier"));
}

// REJECT — a record with no positional fields (a bare `record R; end R;`
// shape, §9) declaring only an invariant member: the record-vs-class
// rejection must not be sensitive to whether the record also has
// positional fields.
BEZEL_TEST(record_static_class_invariant_4186,
           fieldless_record_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "record Empty4186 invariant(true); end Empty4186; "
        "class C4186c public static method Main(): int "
        "  return 0; "
        "end Main; end C4186c;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "record `Empty4186` may not declare an `invariant` member"));
}

// =====================================================================
// ACCEPT — legitimate uses of `invariant` (non-static class bodies), and
// legitimate record/static-class member forms without `invariant`, must
// keep compiling (LANE_RULES §15 — a fix that adds a new rejection must not
// become a false reject of what the spec actually admits).
// =====================================================================

// ACCEPT — the spec's own §22.16 worked example (`Account`), verbatim in
// shape: an ordinary (non-static) class invariant must be entirely
// unaffected by a record/static-class-only check.
BEZEL_TEST(record_static_class_invariant_4186,
           class_invariant_accepted_control_spec_22_16) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Account4186 "
        "  private _balance: decimal := 0m; "
        "  private _limit: decimal := 0m; "
        "  invariant(_balance + _limit >= 0m); "
        "  public constructor(limit: decimal) "
        "    _limit := limit; "
        "  end constructor; "
        "end Account4186; "
        "class C4186d public static method Main(): int "
        "  var a := new Account4186(5m); "
        "  return 42; "
        "end Main; end C4186d;"),
        42);
}

// ACCEPT — a record with ordinary positional fields and a real method, NO
// invariant member, must keep compiling.
BEZEL_TEST(record_static_class_invariant_4186,
           record_ordinary_members_accepted_control_spec_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "record Point4186b "
        "  public X: int; "
        "  public Y: int; "
        "  public method SumIt(): int => X + Y; "
        "end Point4186b; "
        "class C4186e public static method Main(): int "
        "  var p := new Point4186b(20, 22); "
        "  return p.SumIt(); "
        "end Main; end C4186e;"),
        42);
}

// ACCEPT — a static class with ordinary static members, NO invariant
// member, must keep compiling.
BEZEL_TEST(record_static_class_invariant_4186,
           static_class_ordinary_members_accepted_control_spec_11_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "static class Utils4186b "
        "  public static method DoubleIt(x: int): int => x * 2; "
        "end Utils4186b; "
        "class C4186f public static method Main(): int "
        "  return Utils4186b.DoubleIt(21); "
        "end Main; end C4186f;"),
        42);
}

// ACCEPT — a generic record, no invariant member: this check reads only
// `t->kind`/modifiers and the raw member list, so it cannot be sensitive to
// generic parameters.
BEZEL_TEST(record_static_class_invariant_4186,
           generic_record_accepted_control_spec_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "record Box4186<T> public Value: T; end Box4186; "
        "class C4186g public static method Main(): int "
        "  var b := new Box4186<int>(42); "
        "  return b.Value; "
        "end Main; end C4186g;"),
        42);
}

// ACCEPT — a nested static class (declared inside an ordinary class), no
// invariant member: forEachTypeAndNested (#3574) must still reach it and
// find no violation.
BEZEL_TEST(record_static_class_invariant_4186,
           nested_static_class_no_invariant_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Outer4186 "
        "  public static class Inner4186 "
        "    public static method Answer(): int => 42; "
        "  end Inner4186; "
        "end Outer4186; "
        "class C4186h public static method Main(): int "
        "  return Outer4186.Inner4186.Answer(); "
        "end Main; end C4186h;"),
        42);
}
