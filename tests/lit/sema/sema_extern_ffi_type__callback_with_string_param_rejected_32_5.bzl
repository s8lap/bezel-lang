// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, callback_with_string_param_rejected_32_5).
//
// §32.5 / §32.8: a function-type (callback) parameter carrying a non-FFI type
// (`string`) is rejected — its parameters must be FFI-compatible.

module M; extern "c" public method reg(cb: (string) -> int): int; end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `cb` has type `(...) -> int`, which is not FFI-compatible
// CHECK-SAME: (§32.5)
