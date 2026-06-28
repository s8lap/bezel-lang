// Codegen slice CG29c — record `with`-expressions (§9.5, non-destructive
// update). `r with name := value, ... end` evaluates to a copy of the record
// `r` with the named fields replaced; `r` itself is unchanged (records are
// value types). Observable via the int exit code.
//
// Regression: before this slice the `with` path hung the compiler at codegen
// (#254) — the source record value was used directly as a GEP base instead of
// being spilled to a stack slot first.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg29c, with_updates_one_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with x := 9 end; "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 92);
}

BEZEL_TEST(codegen_cg29c, with_does_not_mutate_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with x := 9 end; "
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
        "  var o2 := o with i := new Inner(5) end; "
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
        "  var o2 := o with i := new Inner(5) end; "
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
        "  var o2 := o with i := null end; "
        "  if o2.i = null then return 42; end; "
        "  return o2.i!.v; "
        "end Main; end C;"), 42);
}
