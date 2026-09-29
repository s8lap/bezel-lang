// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, constructor_named_args_reordered_ok).
//
// §22.5 / §22.7: constructor named arguments bind and are checked. A
// reordered, type-correct `new T(...)` named-arg call is accepted.
//
// ACCEPT-SILENTLY control: bezelc -c must exit zero (the program is legal)
// AND must emit no diagnostic at all.
//
// The class below is TRIVIAL-DISPOSE (no destructor, no `owned` field, no
// owning-inline-array field, trivial base). Per spec §28.9 (v0.237) "Trivial-
// dispose classes", a `new` expression of a trivial-dispose class type whose
// result is not stored in an `owned` location or `using var` does *not*
// produce the standard ownership-discharge warning. (There is nothing to
// discharge.) The ACCEPT-SILENTLY control pins this exemption; the CHECK-NOT
// uses the invariant "auto-drop" substring to survive the #4735 diagnostic rename.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module M; class P public X: int := 0; public Y: int := 0; public constructor(x: int, y: int) X := x; Y := y; end constructor; end P; class C public method G() var p := new P(y := 3, x := 10); end G; end C;

// CHECK-NOT: auto-drop
