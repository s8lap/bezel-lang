// fix #2962 — spec §17.4 "Degenerate value-type targets" (v0.241), the unit
// arm — the second half of the SAME paragraph as #2911, under the same recorded
// principle ("`as` requires its result type `T?` to exist and to be
// informative"). §17.4: "The **unit type** is likewise a compile error as an
// `as` target: `()` has exactly one value, so the operation could never carry
// more information than `x is ()` (§12.7), which the diagnostic names as the
// remedy — `()?` remains a well-formed type (§4.7); the rejection is of the
// operator form, not of the type."
//
// The gtest beside this one (tests/sema/as_degenerate_target_2911_2962_test.cpp)
// pins the rejection and its accept-controls; THIS test exists to pin the
// diagnostic's exact words. Two of them carry the ruling's own boundary and are
// the ones a later refactor could drop while every boolean assertion still
// passed: the remedy `x is ()`, and the sentence saying the TYPE `()?` is not
// what is being rejected. The accept side of that boundary is
// as_unit_optional_type_accept_2962.bzl.
//
// Both spellings of the operator form are rejected. §7.10 makes `?` idempotent,
// so `o as ()?` has the same result type `()?` as `o as ()` and carries the same
// (zero) information — it is one operator form written two ways, and admitting
// the second would leave the hole open through one extra character.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class C
  public static method Main(): int
    var o: Object := 7;
    var a := o as ();
    var b := o as ()?;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `as` requires its result type `T?` to exist and to be informative (§17.4), and the unit target `()` has exactly one value — so `x as ()` could never carry more information than the type test `x is ()` (§12.7), which is the remedy.
// CHECK-SAME: The type `()?` itself remains well-formed (§4.7); it is the operator form that is rejected (§17.4/§4.7)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `as` requires its result type `T?` to exist and to be informative (§17.4), and the unit target `()` has exactly one value
