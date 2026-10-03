// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5334 (residual of #5314) / spec §21.6 + §21.1 (v0.313):
// "Locals declared in a block are visible only until the matching `end`",
// and a local's scope is "the innermost enclosing statement block". The
// PROTECTED BLOCK of `try … [catch …] [finally …] end try` (A.6 TryStmt) is
// a statement block, exactly like the `do … end` block of #1242 and the
// `if`/`while`/`case`-arm bodies of #5314.
//
// A local declared in the protected block is therefore NOT visible after the
// matching `end try`. Before the fix sema registered it in the ENCLOSING
// scope, so the read below silently resolved to the leaked local.
//
// MEASURED PRE-FIX (staging r23-c28 96b1642c): this program COMPILED
// (bezelc -c exit 0).
//
// REJECT: a read of a protected-block local after the `end try`.

module M; class C public static method Main(): int try var q := 3; finally Console.WriteLine("f"); end try; return q; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `q`
