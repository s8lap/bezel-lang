// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, unit_param_rejected_32_5).
//
// §32.5: the unit type `()` is a return-only type — not admitted as a
// parameter.

module M; extern "c" public method bad(x: ()): int; end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `x` has type `()`, which is not FFI-compatible
// CHECK-SAME: (§32.5)
