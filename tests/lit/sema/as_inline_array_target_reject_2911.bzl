// fix #2911 — spec §17.4 "Degenerate value-type targets" (v0.241), the
// inline-array arm. §17.4: "An **inline-array target** (any form — `[N]T`,
// `[N]T?`, and the owning forms …) is a **compile error naming this rule**:
// §15.1 admits no nullable inline-array form, so the operator's contract is
// unsatisfiable. The remedy is the `is` test, whose §12.7 narrowing admits
// copying the value out."
//
// The gtest beside this one (tests/sema/as_degenerate_target_2911_2962_test.cpp)
// pins the rejection and its accept-controls; THIS test exists to pin the
// diagnostic's exact words — specifically that it NAMES THE RULE and PRINTS A
// WORKING REMEDY LINE with the user's own type spelled into it. Those are the
// parts a refactor can silently degrade to "invalid `as` target" while every
// boolean assertion still passes.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class C
  public static method Main(): int
    var a: [3]int := [4, 5, 6];
    var o: Object := a;
    var back := o as [3]int;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `as` requires its result type `T?` to exist and to be informative (§17.4), and the inline-array target `[3]int` has no such result: §15.1 admits no nullable inline-array form, so `([3]int)?` — the type §17.4 requires this expression to have — is not a type.
// CHECK-SAME: Use the `is` test instead, whose §12.7 narrowing admits copying the value out: `if o is [3]int then var a: [3]int := o; end;` (§17.4/§15.1)
