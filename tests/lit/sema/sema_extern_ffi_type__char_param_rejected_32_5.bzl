// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, char_param_rejected_32_5).
//
// §32.5: `char` is excluded (use `int`/`byte` and convert).

module M; extern "c" public method foo(c: char): int; end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `c` has type `char`, which is not FFI-compatible
// CHECK-SAME: (§32.5)
