// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, plain_owned_reassign_rejected).
//
// §28.10: reassigning a plain owned-class `using var` local is a compile error.

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Fill() using var x := new R(); x := new R(); end Fill; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot reassign `using var` local `x` — `using var` is implicitly non-reassignable; use a new `do ... end` block for a different resource
// CHECK-SAME: (§28.10)
