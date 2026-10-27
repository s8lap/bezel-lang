// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5314 / spec §7.9 (v0.313): the definite-assignment read check now
// consults the symbol the read resolved to (a read of a PARAMETER or field that
// a nested-block local shadows is not a local read). A genuine LOCAL read must
// still be rejected: `u` (a non-nullable reference local, §7.9) is declared without an initializer and assigned only
// on the `then` path, so the read after the `if` is a §7.9 error.
//
// REJECT: read of a conditionally-assigned local.

module M; class C public static method F(v: int): int var u: string; var s := 0; for var i in 0..<2 do var v := i; s := s + v; end; if s = 1 then u := "a"; end; return u.Length + v; end F; public static method Main(): int return C.F(3); end Main; end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: read of unassigned local `v`
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: read of unassigned local `u`
// CHECK-NOT: read of unassigned local `v`
