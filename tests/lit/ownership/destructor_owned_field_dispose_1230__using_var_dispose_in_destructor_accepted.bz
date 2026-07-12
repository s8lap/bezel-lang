// Ported from tests/ownership/destructor_owned_field_dispose_1230_test.cpp
//   (BEZEL_TEST destructor_owned_field_dispose_1230, using_var_dispose_in_destructor_accepted).
//
// §28.5 (#1230 anti-over-reject): `.Dispose()` on a using-var LOCAL inside a
// destructor is NOT an owned-field call — the local is a new owned resource
// created in the destructor body with no auto-cleanup counterpart. ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner public n: int := 0; destructor end destructor; end Inner; class Outer destructor using var x := new Inner(); x.Dispose(); end destructor; public static method Main(): int return 0; end Main; end Outer;

// NODIAG-NOT: error
// NODIAG-NOT: §
