// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, string_return_rejected_32_5).
//
// §32.5: `string` as a return type is not allowed.

module M; extern "c" public method foo(n: int): string; end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method return type has type `string`, which is not FFI-compatible
// CHECK-SAME: (§32.5)
