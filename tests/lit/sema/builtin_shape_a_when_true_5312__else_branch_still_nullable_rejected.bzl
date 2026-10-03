// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5312 / stdlib §1 (v0.313) + spec §22.4: the `when true` narrowing of a
// builtin shape-(a) Try recipient holds ONLY where the call's bool result is
// known true. In the ELSE branch the recipient is still `int?` (the false path
// assigns null), so `v + 1` there is a §7.11 error. Pins that the fix does not
// over-narrow past the controlled region.
//
// REJECT: arithmetic on the recipient in the else branch.

module M; class C public static method Main(): int var st := new Stack<int>(); if st.TryPop(out var v) then Console.WriteLine(v + 1); else Console.WriteLine(v + 2); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not defined on a nullable value type
// CHECK-NOT: operator `+` is not defined on a nullable value type
