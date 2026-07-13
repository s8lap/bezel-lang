// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, value_primitives_ok_32_5).
//
// §32.5: `bool`, `byte`, `float`, `double` are all FFI-compatible primitives.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; extern "c" public method f(b: bool, n: byte, x: float): double; end;

// NODIAG-NOT: error
// NODIAG-NOT: §
