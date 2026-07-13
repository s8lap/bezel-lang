// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, class_with_field_and_dispose_diagnoses).
//
// Also rejected when the class has other members alongside Dispose().

module M; class Handle _fd: int; public method Dispose() _ := 0; end Dispose; end Handle;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected member visibility (public, private, internal, or protected) or member-kind keyword (method, constructor, destructor)
