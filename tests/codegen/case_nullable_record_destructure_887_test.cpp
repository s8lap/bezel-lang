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
//
// Also covers issue #1254: a positional-destructuring pattern over a CONCRETE
// GENERIC-RECORD instantiation `Wrapper<Item>` (T = a class) produced the same
// "case.end has no terminator" IR-verification failure. The root cause: patternMatches
// ran recordStruct without a type-param substitution (so the unspecialized i32
// placeholder struct was used), and lowerCaseExpr lowered the arm body without the
// substitution (so fieldAddr could not resolve T-typed field accesses like `x.v`
// where x: T — sema typed x as the bare param T, not the concrete Item). The fix
// installs RecordSubstGuard(scrutTy) in both patternMatches and lowerCaseExpr
// so the specialized struct layout and type substitution are active. §19.3 / §14.1.

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
        "  return case p of "
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
        "  return case p of "
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
        "  return case t of "
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
        "  case p of "
        "    Point(x, y) => do return x * 10 + y; end, "  // 81
        "    _ => do return 0; end, "
        "  end; "
        "  return -2; "
        "end Main; end C;"), 81);
}

// ── issue #1254 / §19.3 / §14.1: generic-record positional destructuring ──

// §19.3: a case arm `Wrapper(x) => x.v` over a concrete generic-record
// instantiation `Wrapper<Item>` (T = class) must compile and bind `x` to the
// Item value; `x.v` must read the field through the concrete type. Before the
// fix, the arm body lowering had no active type-param substitution, so
// fieldAddr could not resolve `v` on the bare type-param `T`, returned null,
// and lowerCaseExpr returned null — leaving `case.end` without a terminator
// (IR verification failure). §19.3 / §14.1 / fix #1254.
BEZEL_TEST(case_nullable_record_destructure_887, generic_record_destructure_1254) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Item "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "  destructor end destructor; "
        "end Item; "
        "record Wrapper<T>(value: T); "
        "class C public static method Main(): int "
        "  using var item := new Item(99); "
        "  var w := new Wrapper<Item>(item); "
        "  return case w of "
        "    Wrapper(x) => x.v, "  // 99
        "    _ => -1 "
        "  end; "
        "end Main; end C;"), 99);
}

// §19.3: two-field generic record `Pair<A,B>` with both type params bound to
// a class — both bound names must resolve their fields through the concrete
// types. Also verifies the specialized struct layout (two ptr fields) is used
// for field extraction. §19.3 / §14.1 / fix #1254.
BEZEL_TEST(case_nullable_record_destructure_887, generic_record_two_fields_1254) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box "
        "  public n: int; "
        "  public constructor(n: int) this.n := n; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  using var a := new Box(10); "
        "  using var b := new Box(32); "
        "  var p := new Pair<Box, Box>(a, b); "
        "  return case p of "
        "    Pair(x, y) => x.n + y.n, "  // 42
        "    _ => -1 "
        "  end; "
        "end Main; end C;"), 42);
}
