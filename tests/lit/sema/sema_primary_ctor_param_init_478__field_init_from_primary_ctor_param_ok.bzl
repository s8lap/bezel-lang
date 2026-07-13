// Ported from tests/sema/primary_ctor_param_init_478_test.cpp
//   (BEZEL_TEST sema_primary_ctor_param_init_478, field_init_from_primary_ctor_param_ok).
//
// §22.8: field (not property) initialized from a primary-ctor param is also OK.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box(value: int) public _stored: int := value; end Box;

// NODIAG-NOT: error
// NODIAG-NOT: §
