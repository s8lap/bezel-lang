// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5312 / spec §22.4 (v0.313): "an undeclared method keeps requiring the
// explicit `!`, so no signature silently gains a postcondition its body was
// never checked against." A USER type named `Stack` (shadowing the builtin)
// whose `TryPop(out item: int?)` declares NO `when true` clause is not
// narrowed by the builtin shape-(a) rule: `v + 1` is still a §7.11 error.
//
// REJECT: a user `Stack.TryPop` without the clause.

module M; public class Stack public method TryPop(out item: int?): bool item := 3; return true; end TryPop; end Stack; class C public static method Main(): int var st := new Stack(); if st.TryPop(out var v) then Console.WriteLine(v + 1); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not defined on a nullable value type
