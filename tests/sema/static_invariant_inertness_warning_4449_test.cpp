// Spec-derived tests for §22.16 (v0.338, proposal 0184, #5939): an invariant
// member takes NO `static` qualifier. `static invariant(E);` is a compile
// error at the member — the clause was inert from v0.203 and warned from
// v0.303 (fix #4449), and the `assumed` tier that would have given it a
// meaning is retired. §22.16 "Class invariants", v0.338: "An invariant member
// takes no `static` qualifier: `static invariant(E);` is a compile error at
// the member, the invariant being checked, never proven, in this revision."
//
// The #4449 warning ("`static invariant` is not instrumented in this
// revision") is gone, so every program that used to compile with that
// warning is now a REJECT row. The rejection is emitted by the parser
// (src/parser/Parser_Decls.cpp, InvariantDecl arm), so the needle below is
// the parser's diagnostic text.
//
// Rejection needle (the parser diagnostic, §22.16 v0.338):
//   "an invariant member takes no `static` qualifier"
//
// The one program with no `static` qualifier (plain `invariant(x > 0);`) is
// kept as an ACCEPT control: it must still compile, run, and not be rejected.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kNeedle = "an invariant member takes no `static` qualifier";

int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// ACCEPT control — plain `invariant` (no `static` qualifier) is unaffected.
// =====================================================================

BEZEL_TEST(static_invariant_inertness_warning_4449,
           plain_invariant_still_accepted_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C1 "
        "  public x: int := 1; "
        "  invariant(x > 0); "
        "  public constructor() end constructor; "
        "end C1; "
        "class Main1 public static method Main(): int "
        "  var a := new C1(); "
        "  return 42; "
        "end Main; end Main1;"),
        42);
}

// =====================================================================
// REJECT — each former `static invariant` program is a compile error.
// =====================================================================

// The issue's primary shape: a single `static invariant(true);` clause.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class C2 "
        "  public x: int := 1; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end C2; "
        "class Main2 public static method Main(): int return 0; "
        "end Main; end Main2;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§22.16"));
}

// A plain `invariant` SIDE BY SIDE with a `static invariant`: the static
// clause is rejected; the whole program is rejected at compile time.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_sibling_of_plain_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class C3 "
        "  public x: int := 1; "
        "  invariant(x > 0); "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end C3; "
        "class Main3 public static method Main(): int return 0; "
        "end Main; end Main3;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
}

// Two `static invariant` clauses in one class: each is rejected at its own
// member, so the qualifier diagnostic appears once per clause.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           two_static_invariants_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class C5 "
        "  public x: int := 1; "
        "  static invariant(true); "
        "  static invariant(x > 0); "
        "  public constructor() end constructor; "
        "end C5; "
        "class Main5 public static method Main(): int return 0; "
        "end Main; end Main5;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, kNeedle), 2);
}

// A class with several methods and one `static invariant` clause is still
// rejected once at the member, not per method boundary.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_with_methods_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class C4 "
        "  public x: int := 1; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "  public method A(): int return x; end A; "
        "  public method B(): int return x + 1; end B; "
        "  public method C(): int return x + 2; end C; "
        "end C4; "
        "class Main4 public static method Main(): int return 0; "
        "end Main; end Main4;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, kNeedle), 1);
}

// The #4449 "verdict unchanged" program is now a compile-time rejection, not
// a run to exit 42.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_no_longer_runs_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class Account4449 "
        "  private _balance: decimal := 0m; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end Account4449; "
        "class C4449 public static method Main(): int "
        "  var a := new Account4449(); "
        "  return 42; "
        "end Main; end C4449;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
}

// The #4206 "inert" program with `static invariant(false)` is now rejected.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_false_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class C4449b "
        "  public x: int := 0; "
        "  static invariant(false); "
        "  public constructor() end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      var a := new C4449b(); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C4449b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
}

// Coverage-gap row recorded by #5939 (issue body, "Coverage gap recorded, not
// written"): `static invariant(_n >= 0);` in a class body, the reject shape
// that names the qualifier diagnostic on a private-field predicate.
BEZEL_TEST(static_invariant_inertness_warning_4449,
           static_invariant_private_field_class_body_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class C6 "
        "  private _n: int := 0; "
        "  static invariant(_n >= 0); "
        "  public constructor() end constructor; "
        "end C6; "
        "class Main6 public static method Main(): int return 0; "
        "end Main; end Main6;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
}
