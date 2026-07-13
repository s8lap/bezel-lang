// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, int_params_and_return_ok_32_5).
//
// §32.5: integer types and the value primitives are FFI-compatible.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; extern "c" public method abs(n: int): int; end;

// NODIAG-NOT: error
// NODIAG-NOT: §
