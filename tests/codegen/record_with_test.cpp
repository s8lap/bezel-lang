// Codegen slice CG29c — record `with`-expressions (§9.5, non-destructive
// update). `r with name := value, ... end` evaluates to a copy of the record
// `r` with the named fields replaced; `r` itself is unchanged (records are
// value types). Observable via the int exit code.
//
// Regression: before this slice the `with` path hung the compiler at codegen
// (#254) — the source record value was used directly as a GEP base instead of
// being spilled to a stack slot first.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg29c, with_updates_one_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with(x := 9); "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 92);
}

BEZEL_TEST(codegen_cg29c, with_does_not_mutate_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with(x := 9); "
        "  return a.x * 10 + a.y; "
        "end Main; end C;"), 12);
}

// §9.7 (#1130): a `with`-override of a NULLABLE-record field with a non-null
// record value needs an implicit non-null→nullable widening. The override value
// `new Inner(5)` lowers to a bare `%record.Inner` struct and must be wrapped into
// the field's tagged `{i8, %record.Inner}` representation before the store —
// otherwise the bare payload clobbers the hasValue tag and the value half is left
// holding the SOURCE record's bytes, silently dropping the override (read back as
// the old value 3 instead of 5).
BEZEL_TEST(codegen_cg29c, with_override_nullable_record_field_widens) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(v: int); record Outer(i: Inner?); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(3)); "
        "  var o2 := o with(i := new Inner(5)); "
        "  return o2.i!.v; "
        "end Main; end C;"), 5);
}

// §9.7 (#1130): same widening when the SOURCE field was null — the override must
// still memcpy the new record payload into the slot (not leave it zeroed).
BEZEL_TEST(codegen_cg29c, with_override_nullable_record_field_null_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(v: int); record Outer(i: Inner?); "
        "class C public static method Main(): int "
        "  var o := new Outer(null); "
        "  var o2 := o with(i := new Inner(5)); "
        "  return o2.i!.v; "
        "end Main; end C;"), 5);
}

// §9.7 (#1130) control: overriding a nullable-record field with `null` must
// leave it null (the tag must not be clobbered the other way either).
BEZEL_TEST(codegen_cg29c, with_override_nullable_record_field_to_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(v: int); record Outer(i: Inner?, tag: int); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(3), 9); "
        "  var o2 := o with(i := null); "
        "  if o2.i = null then return 42; end; "
        "  return o2.i!.v; "
        "end Main; end C;"), 42);
}

// #1273 (§9.5 / §7.5): a `with` on a NULLABLE record source (`p: P?`). The source
// lowers to the tagged `{i8, P}` nullable struct; the with-copy must unwrap its
// payload for the field-override copy and re-wrap the result with the source's
// tag. Before the fix the copy treated the `{i8, P}` value as a bare `P`, and a
// later `q!.A` hit nullablePrimHasValue on a wrongly-shaped value → LLVM ICmp
// type-mismatch SIGABRT. Here A is overridden and B is preserved (1→9, 2 kept).
BEZEL_TEST(codegen_cg29c, with_on_nullable_record_source_1273) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p: P? := new P(1, 2); "
        "  var q := p with(A := 9); "
        "  return q!.A * 10 + q!.B; "  // 9*10 + 2 = 92
        "end Main; end C;"), 92);
}

// #1273: a `with` on a NULL nullable-record source propagates null (the source's
// tag rides through), so the result is null — not a crash and not a stale copy.
BEZEL_TEST(codegen_cg29c, with_on_null_record_source_propagates_null_1273) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p: P? := null; "
        "  var q := p with(A := 9); "
        "  return (if q is null then 1 else 0); "
        "end Main; end C;"), 1);
}
