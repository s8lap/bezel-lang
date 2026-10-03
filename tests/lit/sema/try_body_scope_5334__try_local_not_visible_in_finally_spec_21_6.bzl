// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5334 (residual of #5314) / spec §21.6 + §21.1 (v0.313) and §31.2
// (v0.313). The protected block and the `finally` block are SIBLING child
// blocks of the `try` statement, not one enclosing block: a local declared
// in the protected block is out of scope by the time the `finally` runs.
//
// §31.2 sharpens the reason from the runtime side, and it is the same
// reason §7.4 gives the CATCH arms their conservative entry state: the
// `finally` runs on every exit edge of the protected block, including the
// edges taken when the block was INTERRUPTED part-way through. A read of a
// protected-block local in the `finally` is therefore a read of a value
// whose declaration may never have executed — the compile-ACCEPT is a
// read-of-garbage (the #2164/#2116 class) and the compile-REJECT is the
// named diagnostic.
//
// Pre-fix this shape produced "read of unassigned local `q`" ONLY when the
// body could be observed as interrupted; where the accept path held, the
// name resolved and the read was unchecked. The scope rule is the primary
// diagnostic and it does not depend on that path analysis.
//
// MEASURED PRE-FIX (staging r23-c28 96b1642c): this program COMPILED
// (bezelc -c exit 0, no diagnostic).
//
// REJECT: a protected-block local is not visible in the `finally` block.

module M; class C public static method Main(): int try var q := 3; finally Console.WriteLine(q); end try; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `q`
