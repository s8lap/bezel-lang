// Spec-derived behavioral tests for §27.4 (v0.183, #1408) "Lambda-parameter
// record destructuring" and grammar A.7 `LambdaDestructure`. Issue #1587.
//
// §27.4 states: "A lambda parameter may be a positional record destructuring
// in place of an identifier ... `(Entry(k, v)) => body` behaves exactly as
// `(p: Entry) => do var k := p.<first field>; var v := p.<second field>; ...
// body ... end` — each binding is an ordinary read of the corresponding
// positional field at body entry ... Nothing is consumed: the record
// argument is untouched, the bindings own nothing, and the lambda's function
// type is unchanged by the spelling (`(Entry) -> R`)."
//
// These tests verify the desugar actually computes the right values at
// runtime (JIT), across the target-type-known and head-as-annotation forms,
// nesting, and discard slots.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// ── §27.4 example 2: `points.Where((Point(x, _)) => x > 0)` shape, adapted ───
//    to `.Select` (a value-returning LINQ op) so the result is directly
//    checkable. This is the spec's own headline single-param LINQ form — the
//    target parameter type flows through the SINGULAR linqLambdaElementType
//    inference path (§24.3/stdlib §5 D7), not the plural typed-local path.

// §27.4 example 1 exactly: `entries.Select((Entry(k, v)) => $"{k}: {v}")`
// shape — adapted to integers so the result is directly checkable by sum.
// Entry(1,10), Entry(2,20), Entry(3,30) -> k+v per entry -> 11,22,33 -> 66.
BEZEL_TEST(codegen_1587_lambda_destructure, linq_select_headline_example_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var entries: Array<Entry> := "
        "    [new Entry(1, 10), new Entry(2, 20), new Entry(3, 30)]; "
        "  var sums: Array<int> := "
        "    entries.Select((Entry(k, v)) => k + v).ToArray(); "
        "  return sums[0] + sums[1] + sums[2]; "  // 11 + 22 + 33 = 66
        "end Main; end C;"), 66);
}

// ── target type known from a typed local's function-type annotation ─────────

// §27.4: "when a target type supplies the parameter type" — a typed local
// `(Entry) -> int` supplies Entry as the parameter's target type; the
// destructuring head names exactly that type. K=7, V=35 -> 7+35=42.
BEZEL_TEST(codegen_1587_lambda_destructure, target_type_from_typed_local_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(k, v)) => k + v; "
        "  var e := new Entry(7, 35); "
        "  return f(e); "  // 42
        "end Main; end C;"), 42);
}

// Using only the SECOND field (first is discarded implicitly by not being
// referenced) still reads the right positional field.
BEZEL_TEST(codegen_1587_lambda_destructure, reads_second_field_only_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(k, v)) => v; "
        "  var e := new Entry(100, 9); "
        "  return f(e); "  // 9
        "end Main; end C;"), 9);
}

// ── inferred-head case: no target type — "the head IS the annotation" ───────

// §27.4: "when no target type is available, the head *is* the annotation —
// `(Entry(k, v)) => …` alone types the parameter as `Entry`." A top-level
// `var f := (Entry(k, v)) => …;` has no target type context.
BEZEL_TEST(codegen_1587_lambda_destructure, inferred_head_no_target_type_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f := (Entry(k, v)) => k + v; "
        "  var e := new Entry(7, 35); "
        "  return f(e); "  // 42
        "end Main; end C;"), 42);
}

// ── discard slot `_` ──────────────────────────────────────────────────────

// §27.4 / §19.3: `_` in a slot binds nothing; the other slot still binds.
BEZEL_TEST(codegen_1587_lambda_destructure, discard_slot_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var f: (Point) -> int := (Point(_, y)) => y; "
        "  var p := new Point(1000, 17); "
        "  return f(p); "  // 17
        "end Main; end C;"), 17);
}

// ── nested destructuring: `Outer(Inner(a, b), c)` ────────────────────────────

// §27.4: "a nested destructuring meeting the same rules." Outer.I = Inner(a,
// b), Outer.C = c; body computes a + b + c.
BEZEL_TEST(codegen_1587_lambda_destructure, nested_destructuring_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(A: int, B: int); "
        "record Outer(I: Inner, C: int); "
        "class C public static method Main(): int "
        "  var f: (Outer) -> int := (Outer(Inner(a, b), c)) => a + b + c; "
        "  var o := new Outer(new Inner(3, 4), 5); "
        "  return f(o); "  // 3 + 4 + 5 = 12
        "end Main; end C;"), 12);
}

// ── §27.4: "Nothing is consumed: the record argument is untouched" ──────────

// The destructured record argument's own fields are still readable through a
// second call — proves the desugar doesn't consume/mutate the argument.
BEZEL_TEST(codegen_1587_lambda_destructure, argument_not_consumed_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Entry(K: int, V: int); "
        "class C public static method Main(): int "
        "  var f: (Entry) -> int := (Entry(k, v)) => k + v; "
        "  var e := new Entry(2, 3); "
        "  var first := f(e); "
        "  var second := f(e); "  // same `e`, called twice
        "  return first + second; "  // (2+3) + (2+3) = 10
        "end Main; end C;"), 10);
}

// ── class-typed field: a borrow of the argument's field (§28.3) ─────────────

// §27.4: "a borrow of the argument's field for class-typed fields." A record
// with a class-typed positional field; the binding reads a member through it.
BEZEL_TEST(codegen_1587_lambda_destructure, class_typed_field_binding_spec_27_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Widget; "
        "record Holder(W: Widget, Tag: int); "
        "class C public static method Main(): int "
        "  using var w := new Widget(9); "
        "  var f: (Holder) -> int := (Holder(widget, tag)) => widget.Id + tag; "
        "  var h := new Holder(w, 4); "
        "  return f(h); "  // 9 + 4 = 13
        "end Main; end C;"), 13);
}
