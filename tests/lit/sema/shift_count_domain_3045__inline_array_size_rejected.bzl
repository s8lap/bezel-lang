// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §15.1 / §17.14 / §6.5 (v0.242, proposal 0079 / #3045): an inline-array size is
// a constant expression (§15.1: "N must be a constant expression of integer
// type (§17.14)"), so a shift count outside the §6.5 domain makes it a faulting
// constant expression — "a compile error at the constant expression" (§17.14).
// This is the one constant context the expression typechecker does not visit,
// so the size path carries its own diagnostic.
// Before v0.242 the fold masked (40 & 31 == 8) and this was `[256]int` (#1210,
// which introduced the mask to stop `[1 << 40]int` from allocating ~16 GB).

module M; class C public static method Main(): int var a: [1 << 40]int; return a.Length; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an inline-array size shifts by 40, outside the left operand's `0..31` domain
// CHECK-SAME: a constant shift count outside the domain is a compile error at the constant expression (§6.5/§17.14)
// CHECK-SAME: write `(k & 31)` if the modular count is intended
