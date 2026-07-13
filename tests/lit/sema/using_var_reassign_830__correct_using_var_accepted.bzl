// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, correct_using_var_accepted).
//
// A correctly-used `using var` (never reassigned) is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Fill() using var x := new R(); end Fill; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
