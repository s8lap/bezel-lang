// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide_1694, private_base_method_does_not_block_hiding).
//
// Same rule, method form — "not limited to instance members"/field members;
// the blocked-name set filter applies uniformly to every member kind.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base private method Speak() _ := 1; end Speak; end Base; class Derived : Base private method Speak() _ := 2; end Speak; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
