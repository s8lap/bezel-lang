// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5263 / spec §22.4 (v0.313): "Its type is inferred from the method's
// `out` parameter declaration." Convert.TryToDouble's out is `double`
// (stdlib §2.3), so the inline recipient is a `double` and initializing a
// `string` from it is a §5.1 error. Pre-fix the recipient was UNTYPED and this
// program was ACCEPTED (then overflowed the 4-byte slot at run time).
//
// REJECT: `var s: string := d` with `d` from `Convert.TryToDouble(…, out var d)`.

module M; class C public static method Main(): int var ok := Convert.TryToDouble("2.5", out var d); var s: string := d; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `s` has a type that is not implicitly convertible to the target type (§5.1)
