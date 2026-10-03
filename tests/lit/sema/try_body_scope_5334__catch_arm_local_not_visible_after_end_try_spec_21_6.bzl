// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5334 (residual of #5314) / spec §21.6 + §21.1 (v0.313) and §31.2.2
// (v0.313). A CATCH ARM BODY is the third child block of a `try`
// (`ArmBody ::= "do" Statement* "end"`, A.6). Its locals end with the arm,
// not with the `end try`.
//
// MEASURED PRE-FIX (staging r23-c28 96b1642c): ALREADY REJECTED with
// "unresolved identifier `w`" — the arm's `armScope` predates #5314. This
// row PINS that already-correct behaviour: the #5334 fix must not regress
// the catch-arm half of the shape, which was never part of the leak.
//
// REJECT: a catch-arm local is not visible after the `end try`.

module M; class C public static method Main(): int try Console.WriteLine("t"); catch Exception => do var w := 7; Console.WriteLine(w); end end try; return w; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `w`
