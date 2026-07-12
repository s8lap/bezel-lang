// Ported from tests/sema/call_arg_types_test.cpp
//   (BEZEL_TEST sema_argty, derived_arg_to_base_param_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base end Base; class Derived : Base end Derived; class C public method F(b: Base) end F; public method M() this.F(new Derived()); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
