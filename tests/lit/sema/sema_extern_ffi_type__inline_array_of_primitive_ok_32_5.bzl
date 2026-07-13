// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, inline_array_of_primitive_ok_32_5).
//
// §32.5: inline arrays `[N]T` of an FFI-compatible primitive are admitted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; extern "c" public method h(buf: [4]int): int; end;

// NODIAG-NOT: error
// NODIAG-NOT: §
