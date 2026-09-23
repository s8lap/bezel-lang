// Ported from tests/sema/reference_equals_value_warning_1126_test.cpp
//   (BEZEL_TEST sema_reference_equals_value_warning_1126, enum_arg_warns_1126).
//
// §8.4 / §10: an enum value-type argument warns.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; enum Color Red, Green, Blue; end Color; class C public method M(a: Color, b: Color): bool => Object.ReferenceEquals(a, b); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} `Object.ReferenceEquals` is called with a value-type argument; a non-null value is boxed into a fresh object on every call, so the comparison is always false for it — a null nullable value boxes to a null reference instead and compares true only against another null
// CHECK-SAME: (§8.4/§8.6)
