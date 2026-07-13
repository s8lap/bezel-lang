// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, plain_var_increment_not_flagged_913).
//
// A plain `var` VALUE increment is NOT flagged by the §28.10 rule, and (since
// #913's plain-var slice) is NOT §28.3-rejected either — an `int` is not owned.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Main(): int var x := 3; x++; return x; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
