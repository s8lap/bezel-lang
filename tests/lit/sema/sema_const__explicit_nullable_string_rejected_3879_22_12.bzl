// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3879 (§22.12, v0.294): `string` / `string?` has its own row in rule
// 2's first table ("a reference type ([§3.1](#sec-type-kinds)); rule 3"),
// whose `const` column states the identical non-nullable-ok/nullable-reject
// split: "`string`: ok; `string?`: —". `string` is represented as a
// `TypeRef::Kind::Primitive` (PrimitiveType::String) in this compiler, so the
// same missing-`nullable`-check gap this issue fixes for `int?` also silently
// accepted `public const S: string? := "hi";` before the fix.

module M; class C public const S: string? := "hi"; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `S` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
