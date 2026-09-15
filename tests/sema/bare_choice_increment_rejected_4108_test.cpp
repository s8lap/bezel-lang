// Spec-derived regression test for issue #4108 (§17.6 "The increment and
// decrement operators", v0.295).
//
// THE DEFECT. §17.6 admits `x++`/`x--` only on three operand categories:
// (1) a value-type operand whose type T overloads `operator ++`, (2) a
// built-in numeric type, (3) a reference-type operand whose type overloads
// the operator (§4.6). A `choice`-typed operand is none of these.
//
// Before the fix, `ExprTypecheck.cpp`'s IncrementStmt handler's
// `isRefType`/`isValueTypeRecord` classification never matched
// `TypeDeclKind::Choice`, so no #3978-style rejection fired. The statement
// was accepted, compiled clean, and silently no-opped at runtime: the
// choice value was unchanged after the statement.
//
// MEASURED on the pre-fix compiler (R17 integration head 2504380):
// `bezelc -c -o /dev/null` on the issue's repro -> rc=0, zero diagnostics.
// `./prog` -> exit code 3 (the original Circle(3) payload, unchanged).
//
// The fix: extend the unconditional-reject branch in ExprTypecheck.cpp's
// IncrementStmt handler to also cover choice-typed operands.
//
// ===========================================================================
// REJECT tests — these FAIL without the fix, PASS with it
// ===========================================================================

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool diagContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

int run(const JitResult& r) { return r.ran ? r.exitCode : -999; }

}  // namespace

// T1: bare `choice++` on a choice-typed local — the issue's primary repro.
BEZEL_TEST(bare_choice_increment_rejected_4108,
           bare_choice_plusplus_on_local_rejected_spec_17_6) {
    auto r = runAot(
        "module M; "
        "choice Shape "
        "  Circle(r: int), "
        "  Square(s: int); "
        "end Shape; "
        "class C "
        "  public static method Main(): int "
        "    var sh := Shape.Circle(3); "
        "    sh++; "
        "    return (case sh of Circle(r) => r, Square(_) => -1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // Should cite §17.6 — the section that governs increment/decrement.
    bool hasDiag = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("++") != std::string::npos &&
            d.message.find("17.6") != std::string::npos) {
            hasDiag = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(hasDiag);
}

// T2: bare `choice--` on a choice-typed local.
BEZEL_TEST(bare_choice_increment_rejected_4108,
           bare_choice_minusminus_on_local_rejected_spec_17_6) {
    auto r = runAot(
        "module M; "
        "choice Color "
        "  Red, "
        "  Green, "
        "  Blue; "
        "end Color; "
        "class C "
        "  public static method Main(): int "
        "    var c := Color.Red; "
        "    c--; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "--"));
}

// T3: `choice++` on a field.
BEZEL_TEST(bare_choice_increment_rejected_4108,
           bare_choice_plusplus_on_field_rejected_spec_17_6) {
    auto r = runAot(
        "module M; "
        "choice Shape "
        "  Circle(r: int), "
        "  Square(s: int); "
        "end Shape; "
        "class C "
        "  public s: Shape := Shape.Circle(1); "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    c.s++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ===========================================================================
// ACCEPT controls — none of these should be affected by the fix.
// ===========================================================================

// T4: `int` local — §17.6 category (2).
BEZEL_TEST(bare_choice_increment_rejected_4108,
           int_local_increment_still_accepted_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var i := 5; i++; return i; "
        "end Main; end C;")), 6);
}

// T5: `char` local.
BEZEL_TEST(bare_choice_increment_rejected_4108,
           char_local_increment_still_accepted_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var ch: char := 'a'; ch++; return int(ch); "
        "end Main; end C;")), 98);
}

// T6: enum local — §17.6 does not govern enums; their numeric representation
// makes `e++`/`e--` valid when the underlying value is in range.
BEZEL_TEST(bare_choice_increment_rejected_4108,
           enum_local_increment_still_accepted) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "enum E: int X, Y, Z; "
        "end E; "
        "class C public static method Main(): int "
        "  var e := E.X; e++; return int(e); "
        "end Main; end C;")), 1);
}