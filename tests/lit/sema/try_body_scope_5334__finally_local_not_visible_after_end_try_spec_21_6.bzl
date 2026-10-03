// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5334 (residual of #5314) / spec §21.6 + §21.1 (v0.313):
// "Locals declared in a block are visible only until the matching `end`".
// The FINALLY BLOCK (`FinallyClause ::= "finally" Statement*`, A.6) is the
// third child block of a `try` statement, sibling to the protected block
// and to each catch arm body. Its locals end with the matching `end try`.
//
// This is the ISSUE'S OWN REPRO: `try var q := 3; finally var z := 5; end
// try; return z;` compiled pre-fix and the run printed `32579` — `z` read
// garbage out of a stale stack slot, because the leaked local was accepted
// and the name resolved to it.
//
// MEASURED PRE-FIX (staging r23-c28 96b1642c): this program COMPILED
// (bezelc -c exit 0, no diagnostic).
//
// REJECT: a read of a finally-block local after the `end try`.

module M; class C public static method Main(): int try var q := 3; finally var z := 5; end try; return z; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `z`
