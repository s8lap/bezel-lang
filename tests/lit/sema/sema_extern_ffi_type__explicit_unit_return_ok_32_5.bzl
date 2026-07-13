// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, explicit_unit_return_ok_32_5).
//
// §32.5: the unit type `()` is valid as a return type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; extern "c" public method g(n: int): (); end;

// NODIAG-NOT: error
// NODIAG-NOT: §
