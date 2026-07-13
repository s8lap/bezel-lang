// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, using_var_increment_rejected_913).
//
// fix #913 / §28.10 / §17.6: `c++` / `c--` desugars to `c := T.operator++(c)` —
// a reassignment — so it is rejected on a non-reassignable `using var` local, the
// same as a direct `:=`. Before the fix it was silently accepted and (for an
// owned-class operand whose `operator ++` returns a new instance) leaked the
// previous value (§28.7).

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Main(): int using var x := 3; x++; return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot increment/decrement `using var` local `x` — `using var` is implicitly non-reassignable and `++` reassigns it
// CHECK-SAME: (§28.10 / §17.6)
