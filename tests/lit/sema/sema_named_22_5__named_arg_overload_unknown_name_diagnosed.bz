// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, named_arg_overload_unknown_name_diagnosed).
//
// §22.5: an unknown parameter name in an overloaded call binds to no
// candidate, so resolution finds no applicable overload (the call is not
// silently accepted). At minimum it must surface a diagnostic for the bad
// name on the sole-arity candidate path.

module M; class C public method F(value: int): int => value; public method G(): int return this.F(nope := 3); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `F` has no parameter named `nope`
// CHECK-SAME: (§22.5)
