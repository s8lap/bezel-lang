// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, constructor_named_args_reordered_ok).
//
// §22.5 / §22.7: constructor named arguments bind and are checked. A
// reordered, type-correct `new T(...)` named-arg call is accepted.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class P public X: int := 0; public Y: int := 0; public constructor(x: int, y: int) X := x; Y := y; end constructor; end P; class C public method G() var p := new P(y := 3, x := 10); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `p` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
