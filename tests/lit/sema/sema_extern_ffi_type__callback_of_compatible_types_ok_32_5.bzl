// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, callback_of_compatible_types_ok_32_5).
//
// §32.5 / §32.8: a function-type (callback) parameter whose params and return
// are themselves FFI-compatible is admitted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; extern "c" public method reg(cb: (int, int) -> int): int; end;

// NODIAG-NOT: error
// NODIAG-NOT: §
