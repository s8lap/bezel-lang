// fix #3434 (codegen; language spec §4.7 "The unit type" / §21.7 "The `return`
// statement", v0.283): the NEGATIVE control for the accept test in this
// directory, arrow_body_declared_unit_return_no_degrade_note_3434.bzl — and the
// sharper of the two controls this fix has.
//
// #3434 replaces #2963's `!declRet` SPELLING test with a semantic one: is the
// callable's return type the unit type? The obvious way to get that wrong is to
// treat anything unit-shaped as exempt. §4.7 forbids it: "Wrapping the unit type
// with `?` (yielding `()?`) is permitted by the grammar and produces an optional
// with two values, `null` and `()`." A `()?`-returning callable therefore DOES
// return a value, and an arrow body that produced none there really did degrade
// — `emitReturn`'s `v ? v : null` fallback substitutes the ABSENT `()?`, which
// §4.7 makes a different value from the `()` the body computed. The note is
// telling the truth, and must keep being emitted.
//
// This is the same distinction #3095 had to make on the sema side, where
// `returnsValue`'s `!r->nullable` conjunct stops a `()?` ascription from folding
// into the no-return-type case. Both sides now read it from the ONE predicate
// (`sema::ascribedReturnTypeProducesValue`), which is why one control pins both.
//
// `ArrowNullableUnit` is CALLED, because an uninstantiated class's body is never
// emitted and the note would then be absent for a reason that has nothing to do
// with this fix.
//
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public static method Nothing() Console.WriteLine("side"); end Nothing;
  public static method ArrowNullableUnit(): ()? => C.Nothing();
  public static method Main(): int
    var r := C.ArrowNullableUnit();
    return (if r = null then 1 else 0);
  end Main;
end C;

// CHECK: internal: return-value expression failed to lower to a value
// CHECK-SAME: #2491
