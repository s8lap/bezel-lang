// Ported from tests/ownership/using_var_return_762_test.cpp
//   (BEZEL_TEST using_var_return_762, using_var_scope_discharge_accepted).
//
// §28.2: a `using var` that is simply used and discharged at scope end (no
// return) is the normal, legal pattern — no diagnostic. ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget public n: int := 0; destructor end destructor; public static method Use(): int using var w := new Widget(); return w.n; end Use; public static method Main(): int return 0; end Main; end Widget;

// NODIAG-NOT: error
// NODIAG-NOT: §
