// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §6.6 (A.6), v0.196 (#1744): grammar A.6 restricts an `LValue`'s base to
// `Identifier | "this"`. A `new` expression is a fresh value, not a storage
// location, so `new C().field := v` bases the assignment chain on a NewExpr —
// which A.6 admits no more than a method-call result. Sibling of #1690 (which
// rejects a CallExpr / NullCondCallExpr base): the #1690 check walked past the
// `.field` hop to the base but only matched call-shaped bases, so this NewExpr
// base slipped through and the ill-formed assignment compiled clean.

module M; class C public total: int := 0; public static method Main(): int new C().total := 5; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a newly-constructed object cannot be the base of an assignment target — grammar A.6's lvalue base must be an identifier or `this`; bind the `new` expression to a local first (`var c := new C(); c.items[0] := v;`)
// CHECK-SAME: (§6.6)
