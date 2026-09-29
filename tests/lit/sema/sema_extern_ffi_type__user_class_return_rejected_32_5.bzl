// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/extern_ffi_type_test.cpp
//   (BEZEL_TEST sema_extern_ffi_type, user_class_return_rejected_32_5).
//
// §32.5: a user class as the return type is not an FFI-boundary type.

module M; class Widget public method M() return; end M; end Widget; extern "c" public method bar(n: int): Widget; end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method return type has type `Widget`, which is not FFI-compatible
// CHECK-SAME: (§32.5)
