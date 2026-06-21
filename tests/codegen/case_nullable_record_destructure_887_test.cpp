// Codegen — issue #887: a `case` with a record DESTRUCTURING pattern over a
// NULLABLE record scrutinee (`p: Point?`) produced an unterminated basic block →
// IR verification failure. The nullable-value branch of patternMatches handled
// Null/Named/Constant/Binding/Range patterns but not DestructuringPattern, so it
// fell through to the "deferred" null return and the case lowering emitted a
// block with no terminator.
//
// THE FIX: in the nullable-value branch, unwrap the record payload
// (nullablePrimValue) and run the non-nullable destructuring extraction on it,
// AND-ed with hasValue — so the pattern matches only a present value and binds
// the field slots from the record's fields. §19.3 / §9.4 / §7.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// A present nullable record destructures and binds its fields.
BEZEL_TEST(case_nullable_record_destructure_887, present_value_destructures) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p: Point? := new Point(3, 4); "
        "  return case p "
        "    Point(x, y) => x + y, "  // 7
        "    _ => 99 "
        "  end; "
        "end Main; end C;"), 7);
}

// A null nullable record does NOT match the destructuring pattern (§19.5: typed
// patterns never match null) → the `_` arm fires.
BEZEL_TEST(case_nullable_record_destructure_887, null_value_falls_to_wildcard) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p: Point? := null; "
        "  return case p "
        "    Point(x, y) => x + y, "
        "    _ => 42 "  // 42
        "  end; "
        "end Main; end C;"), 42);
}

// A three-field nullable record destructures all positions correctly.
BEZEL_TEST(case_nullable_record_destructure_887, three_field_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Triple(A: int, B: int, C: int); "
        "class C public static method Main(): int "
        "  var t: Triple? := new Triple(10, 20, 5); "
        "  return case t "
        "    Triple(a, b, c) => a + b + c, "  // 35
        "    _ => 0 "
        "  end; "
        "end Main; end C;"), 35);
}

// Statement-position case over a nullable record also lowers cleanly.
BEZEL_TEST(case_nullable_record_destructure_887, statement_position_case) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p: Point? := new Point(8, 1); "
        "  case p "
        "    Point(x, y) => do return x * 10 + y; end, "  // 81
        "    _ => do return 0; end, "
        "  end; "
        "  return -2; "
        "end Main; end C;"), 81);
}
