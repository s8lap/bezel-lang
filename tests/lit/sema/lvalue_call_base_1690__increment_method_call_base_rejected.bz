// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/lvalue_call_base_1690_test.cpp
//   (BEZEL_TEST lvalue_call_base_1690, increment_method_call_base_rejected).
//
// §6.6 / A.6 (`IncrementStmt ::= LValue ("++"|"--")`): the same restriction
// applies to `++`/`--`, which consume the same grammar A.6 `LValue`
// production as AssignmentStmt.

module M; class C public total: int := 0; public method Self(): C => this; public static method Main(): int using var h := new C(); h.Self().total++; return h.total; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a method-call result cannot be the base of an assignment target — grammar A.6's lvalue base must be an identifier or `this`; extract the call result into a local first (`var c := getCache(); c.items[0] := v;`)
// CHECK-SAME: (§6.6)
