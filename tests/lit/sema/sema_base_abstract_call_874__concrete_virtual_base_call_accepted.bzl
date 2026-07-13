// Ported from tests/sema/base_abstract_call_874_test.cpp
//   (BEZEL_TEST sema_base_abstract_call_874, concrete_virtual_base_call_accepted).
//
// §12.3: calling a CONCRETE (virtual) base method via `base.M()` is valid
// and must not be rejected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual method Compute(): int => 10; end Base; class Derived : Base public override method Compute(): int var b: int := base.Compute(); return b; end Compute; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
