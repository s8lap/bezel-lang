// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5263 / #5271 / spec §22.4 (v0.313): the inline recipient of
// `decimal.TryParse(s, out var d)` takes the out parameter's declared type
// `decimal` (stdlib §9.3), so initializing a `string` from it is a §5.1 error.
// Pre-fix the recipient was UNTYPED and this program was ACCEPTED.
//
// REJECT: `var s: string := d` with `d` from `decimal.TryParse(…, out var d)`.

module M; class C public static method Main(): int var ok := decimal.TryParse("1.25", out var d); var s: string := d; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `s` has a type that is not implicitly convertible to the target type (§5.1)
