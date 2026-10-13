// Spec-derived tests for issue #4206, updated for §22.16 v0.338 (proposal
// 0184, #5939). Before v0.338, a `static invariant` inside an ORDINARY class
// compiled and was inert (§22.16 "Out of scope in this revision", v0.298).
// v0.338 retires the `"static"?` qualifier on an invariant member: §22.16
// "Class invariants" now reads "An invariant member takes no `static`
// qualifier: `static invariant(E);` is a compile error at the member, the
// invariant being checked, never proven, in this revision."
//
// Each program below was an ACCEPT row before #5939 and is now a REJECT row.
// The rejection is the parser's diagnostic (src/parser/Parser_Decls.cpp,
// InvariantDecl arm), so the needle is the parser's text.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kNeedle = "an invariant member takes no `static` qualifier";

}  // namespace

// REJECT — `static invariant(true);` in an ordinary class (the #4206 repro).
BEZEL_TEST(static_invariant_ordinary_class_4206,
           static_invariant_true_ordinary_class_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class Account4206 "
        "  private _balance: decimal := 0m; "
        "  static invariant(true); "
        "  public constructor() end constructor; "
        "end Account4206; "
        "class C4206 public static method Main(): int "
        "  var a := new Account4206(); "
        "  return 42; "
        "end Main; end C4206;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§22.16"));
}

// REJECT — `static invariant(false);` with a method (the former inert case).
BEZEL_TEST(static_invariant_ordinary_class_4206,
           static_invariant_false_ordinary_class_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class Account4206b "
        "  private _balance: decimal := 0m; "
        "  static invariant(false); "
        "  public constructor() end constructor; "
        "  public method Bump(): int "
        "    _balance := _balance + 1m; "
        "    return 7; "
        "  end Bump; "
        "end Account4206b; "
        "class C4206b public static method Main(): int "
        "  var r: int := 0; "
        "  try "
        "    var a := new Account4206b(); "
        "    r := a.Bump(); "
        "  catch ContractViolationException => r := 99; "
        "  end; "
        "  return r; "
        "end Main; end C4206b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
}

// REJECT — a `static invariant(false);` beside a plain `invariant`: the
// static member is rejected; the plain sibling does not rescue the program.
BEZEL_TEST(static_invariant_ordinary_class_4206,
           static_invariant_sibling_real_invariant_rejected_spec_22_16) {
    auto r = runAot(
        "module M; "
        "class Account4206c "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  static invariant(false); "
        "  public constructor(v: int) x := v; end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var a := new Account4206c(-1); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end Account4206c;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
}
