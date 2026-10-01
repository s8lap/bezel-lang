// fix #1206 / §19.3 / §19.5: a bare RECORD (or ENUM) type-name pattern in a
// `case` expression emitted malformed IR — `patternMatches` NamedPattern handler
// returned nullptr for a value-type name (records/enums have no vtable descriptor
// so `descriptorForPatternName` returned nullptr), leaving the `case.end` basic
// block without a terminator (IR verification failure / compile error).
//
// A record is a sealed value type with no subtyping (§9.6); a bare type-name
// pattern `Pt` over an unboxed record scrutinee is a static compile-time test:
// if the scrutinee's static type IS Pt the arm always matches; any other concrete
// type means the arm never matches.  Same reasoning applies to a bare enum type
// name (§10).  Mirrors the BindingPattern typed-binding branch that already
// handled `var q: Pt =>`.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.5 (#1206 repro): bare record type-name pattern over a matching scrutinee.
// Previously: IR_FAIL (unterminated case.end).  Now: compiles and returns 7.
BEZEL_TEST(case_record_bare_type_pattern_1206, record_bare_type_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(1, 2); "
        "  return (case p of Pt => 7, _ => 0); "  // Pt matches → 7
        "end Main; end C;"), 7);
}

// §19.3 / Appendix C.1 (v0.313, #5188) FLIP (was: never matches, wildcard
// fires, exit 0): a bare record type-name pattern over a DIFFERENT record type
// can never succeed and is now a compile error. The same-type row above is
// unchanged.
BEZEL_TEST(case_record_bare_type_pattern_1206, record_bare_type_nonmatch_rejected_5188) {
    auto r = runAot(
        "module M; record Pt(x: int, y: int); record Qt(a: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(1, 2); "
        "  return (case p of Qt => 5, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "naming a different value type"));
}

// §10 (#1206): a bare ENUM type-name pattern over an enum scrutinee.
// An enum value always matches the bare enum type name.
BEZEL_TEST(case_record_bare_type_pattern_1206, enum_bare_type_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum E A, B, C; end E; "
        "class C public static method Main(): int "
        "  var e := E.B; "
        "  return (case e of E => 42, _ => 0); "  // E matches → 42
        "end Main; end C;"), 42);
}

// §19.3 / §19.5 (#1206): regression — a bare record type-name pattern does not
// disturb an existing typed-binding pattern (`var q: Pt`) in the same file.
// Both the bare and the typed-binding form must continue to work together.
BEZEL_TEST(case_record_bare_type_pattern_1206, bare_and_typed_binding_coexist) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); "
        "  var a := (case p of var q: Pt => q.x + q.y, _ => 0); "  // typed binding → 7
        "  var b := (case p of Pt => 1, _ => 0); "                  // bare type   → 1
        "  return a * 10 + b; "  // 71
        "end Main; end C;"), 71);
}
