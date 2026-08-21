// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not defined on a nullable value type — narrow first

// Ported from tests/sema/lifted_operators_486_sema_test.cpp
// (BEZEL_TEST ['int', 'add']int_add_result_needs_narrowing_spec_7_11).
//
// #3298 / v0.267 / proposal 0114 (override of pre-v0.267 lift acceptance):
// the operator is now REJECTED for a nullable value type operand (§7.11).

module M; class C
  public method M(a: int?, b: int?): int
    var s := a + b;
    return s;
  end M;
end C;
