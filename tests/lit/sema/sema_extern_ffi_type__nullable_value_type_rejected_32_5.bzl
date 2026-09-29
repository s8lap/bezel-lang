// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for issue #1715 (spec §32.5, v0.196).
//
// §32.5 "Nullable boundary forms": "Nullable value types are rejected:
// `int?`, `bool?`, `IntPtr?`, `Ptr<T>?`, and nullable record forms have no C
// representation." Only the pointer-carrying boundary forms admit `?` (see the
// companion accept test). The Named case of `isFfiCompatibleType` previously
// ignored `t.nullable`, so a nullable primitive and `IntPtr?`/`UIntPtr?` were
// wrongly accepted at the boundary. Both must now be rejected, the diagnostic
// naming the offending type.

module M;
extern "c"
  public method takes_int_opt(x: int?): int;
  public method takes_intptr_opt(x: IntPtr?): int;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `x` has type `int?`, which is a nullable value type — nullable value types are rejected at the FFI boundary
// CHECK-SAME: (§32.5)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `x` has type `IntPtr?`, which is a nullable value type — nullable value types are rejected at the FFI boundary
// CHECK-SAME: (§32.5)
