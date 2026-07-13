// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, discard_assignment_not_flagged).
//
// The `_` discard (`_ := expr`) is not a `using var` rebind and must not be
// flagged by the §28.10 rule.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Main(): int _ := 99; return 5; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
