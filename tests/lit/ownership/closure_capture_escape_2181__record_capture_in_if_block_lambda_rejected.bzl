// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §28.9 (v0.220, issue #2181): a borrow-bearing record captured only
// UNDER an `if` inside a block-bodied lambda that is returned. Before the fix,
// collectIdentifiersInStmt walked only the leaf statement kinds of a lambda
// block body (ExpressionStmt / ReturnStmt / LocalDecl / AssignmentStmt /
// DiscardStmt) and NOT the control-flow forms, so a capture appearing only
// inside an `if`/`while`/`for` of a block lambda was invisible to EVERY
// capture-escape leg — the §9.7 record leg included — and the program compiled
// and read the dangling span once `local` auto-dropped (§28.3). The exact
// repro from the issue. // per §9.7 (v0.220), §28.3

module M; record Holder(S: ArraySpan<int>); class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); return lambda(): int => do if h.S.Length > 0 then return h.S[0]; end; return 0; end; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `h`, a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 closure-capture rule
