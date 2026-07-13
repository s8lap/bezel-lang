// Ported from tests/sema/base_abstract_call_874_test.cpp
//   (BEZEL_TEST sema_base_abstract_call_874, multilevel_concrete_base_call_accepted).
//
// Multi-level hierarchy: B extends A (concrete) and C extends B; calling
// base.Compute() from C where B.Compute() is concrete must be accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public virtual method Compute(): int => 1; end A; class B : A public override method Compute(): int => 2; end B; class C : B public override method Compute(): int var b: int := base.Compute(); return b; end Compute; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
