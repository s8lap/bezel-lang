// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5334 (residual of #5314) / spec §31.2.1 (v0.313): "Matches and
// binds the exception to `name` for use in the arm's guard and body. The
// binding is in scope ONLY WITHIN THE ARM." The catch-arm binding is bound by
// the catch SYNTAX (`var ex: Type`), not declared in the arm body — so it is
// not a §21.6 block local, and this row pins that the #5334 child-Scope work
// leaves the binding's existing armScope exactly as §31.2.1 states it.
//
// MEASURED PRE-FIX AND POST-FIX: rejected with "unresolved identifier `ex`"
// at the read past the `end try` (the read inside the arm resolves).
//
// REJECT: the catch-arm binding is not visible after the `end try`.

module M; class C public static method Main(): int try Console.WriteLine("t"); catch var ex: Exception => do Console.WriteLine(ex.Message); end end try; return ex.Message.Length; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `ex`
