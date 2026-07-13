// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, iface_method_overload_provides_arity_ok_r39).
//
// CONTROL: an extra overload alongside the matching one is accepted (the class
// provides SOME method with the required arity).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IGo method Run(x: int): int; end IGo; class Impl : IGo public method Run(): int return 0; end Run; public method Run(x: int): int return x; end Run; end Impl;

// NODIAG-NOT: error
// NODIAG-NOT: §
