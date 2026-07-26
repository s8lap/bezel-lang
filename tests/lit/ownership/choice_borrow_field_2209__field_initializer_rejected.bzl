// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the field-INITIALIZER face of the same
// rule, exercised through `MethodOwnership::checkFieldInitializer` (one of
// the three rejection sites the fix touches — the OTHER two are the bare
// `this.f := …` body assignment, covered by
// choice_borrow_field_2209__instance_field_store_rejected.bzl, and the
// `Add`/`Insert` call sites on Escapes-classified receivers, covered by
// `checkCollectionBorrowStoreEscape`). A class with an instance field whose
// initializer binds a borrow-bearing choice into it (`v: Opt := Opt.Some(shared)`
// where `shared` is a `Res?` borrow-typed static and `Opt`'s `Some(r: Res)`
// payload carries the borrow) is rejected at parse-and-check time. Mirrors
// the parallel record initializer test #1540 (`Holder2.h: RefRec := new
// RefRec(shared)`); the §9.7 field-init rule and the #2209 fix extend it to
// choice-typed fields symmetrically.

module M; class Res public method Val(): int return 5; end Val; end Res; choice Opt Some(r: Res), None; end Opt; class Holder2 public static shared: Res? := null; public v: Opt := Opt.Some(shared); end Holder2; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing choice may not be stored in the instance field `v`
// CHECK-SAME: (§9.7 with "record" read as "record or choice", §35.4
