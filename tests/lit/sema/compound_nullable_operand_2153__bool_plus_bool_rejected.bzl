// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §3.5, spec v0.220): the plain `bool += bool` sibling of the
// nullable cell — the §6.6 family check deliberately excluded a `bool` target
// from the arithmetic branch, so `bool(true) += bool(true)` compiled to an i1
// add and raised OverflowException at RUNTIME (probe, main @dd33abd) instead
// of being rejected: §3.5 defines no arithmetic on `bool`, so `bool + bool`
// has no result for the §6.6 desugaring to store.

module M; class C public static method Main(): int var b: bool := true; var c: bool := true; b += c; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not valid on a `bool` target — the arithmetic operators are not defined on `bool` (§3.5); boolean combinations use `and`/`or`/`not` (§17.2)
