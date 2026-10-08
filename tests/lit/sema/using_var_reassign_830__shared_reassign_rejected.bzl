// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, shared_reassign_rejected).
//
// §28.10: reassigning a `Shared<T>` `using var` is likewise rejected.
// per §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.

module M; class Inner public static n: int := 0; destructor Inner.n := Inner.n + 1; end destructor; end Inner; class C public static method Fill() var i1 := new Inner(); using var h1 := new Shared<Inner>(move i1); var i2 := new Inner(); using var h2 := new Shared<Inner>(move i2); h1 := h2.Clone(); end Fill; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot reassign `using var` local `h1` — `using var` is implicitly non-reassignable; use a new `do ... end` block for a different resource
// CHECK-SAME: (§28.10)
