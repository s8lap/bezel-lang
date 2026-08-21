// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §3.5 / §7.11, spec v0.220): `bool? += bool?` — the arithmetic
// operators are not defined on `bool` (§3.5), so there is no underlying
// operator for §7.11 to lift. Before the fix the nullable gate skipped the
// check and the #1964 lowering added the i1 payloads —
// `bool?(true) += bool?(false)` silently "computed" a boolean sum.

module M; class C public static method Main(): int var b: bool? := true; var c: bool? := false; b += c; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not valid on a `bool?` target — the arithmetic operators are not defined on `bool` (§3.5); boolean combinations use `and`/`or`/`not` (§17.2)
