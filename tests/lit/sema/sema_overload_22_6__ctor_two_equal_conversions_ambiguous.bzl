// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, ctor_two_equal_conversions_ambiguous).
//
// §22.6 / §22.7 — ambiguous constructor call: int argument converts
// equally to long and to float. `new T(a)` with `a: int` is a compile
// error citing the candidate constructors.

module M; class T public constructor(a: long) end constructor; public constructor(a: float) end constructor; end T; class C public method M(a: int) _ := new T(a); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: ambiguous call to `new T`: no overload is unambiguously better
// CHECK-SAME: (§22.6)
