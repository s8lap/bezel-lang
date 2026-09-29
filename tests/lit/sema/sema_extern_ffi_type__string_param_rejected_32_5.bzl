// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, string_param_rejected_32_5).
//
// §32.5: Bezel `string` is not allowed (use `CString`/`WString`).

module M; extern "c" public method foo(s: string): int; end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `s` has type `string`, which is not FFI-compatible
// CHECK-SAME: (§32.5)
