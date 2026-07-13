// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_12_9_type_alias_out_rejected).
//
// §12.9 / §27.8: a type alias's own type parameter may not be marked
// variant.

module M; class Box<T> private _v: T; public method Get(): T => _v; end Box; type Alias<out T> = Box<T>;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type aliases bind with ':=' since v0.160 (replace '=' with ':='; §27.8)
