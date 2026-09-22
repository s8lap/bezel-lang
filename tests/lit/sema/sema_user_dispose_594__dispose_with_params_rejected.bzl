// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, dispose_with_params_ok).
//
// §28.2 (v0.300): "Declaring a member named `Dispose` on a class or a
// record is a compile error at that declaration, **whatever its parameter
// list**." Through v0.299 the rule was scoped to the zero-parameter form
// only; the widening to any parameter list is the ruling's whole content
// (issue #4404, tracked by #4421).
//
// The old ACCEPT control for this program was retired in the same commit
// that flipped the compiler check — inverting it earlier would fail the
// lit track against the pre-fix binary (#4421 sequencing).

module M; class C public method Dispose(reason: string) _ := 0; end Dispose; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `C` may not declare a `Dispose` method directly
// CHECK-SAME: (§28.2)
