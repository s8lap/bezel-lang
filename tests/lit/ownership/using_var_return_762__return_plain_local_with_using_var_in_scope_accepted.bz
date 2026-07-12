// Ported from tests/ownership/using_var_return_762_test.cpp
//   (BEZEL_TEST using_var_return_762, return_plain_local_with_using_var_in_scope_accepted).
//
// §28.3: the rejection keys on the RETURNED identifier, not on the mere presence
// of a `using var` in scope — returning a plain owned local while a `using var`
// guard is live is fine (the guard discharges at scope end). ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Widget destructor end destructor; public static method Ok(): owned Widget using var guard := new Widget(); var result := new Widget(); return result; end Ok; public static method Main(): int return 0; end Main; end Widget;

// NODIAG-NOT: error
// NODIAG-NOT: §
