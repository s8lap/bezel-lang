// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #5334 (residual of #5314) — CONTROL row. Giving the protected block,
// each catch arm body and the `finally` block their own child Scope must not
// break the names that remain legitimately visible: an OUTER local declared
// before the `try` is still readable in all three bodies and after the
// `end try` (§21.1 — a child scope's parent is the enclosing block).
//
// MEASURED PRE-FIX AND POST-FIX: accepted, no diagnostic.
//
// ACCEPT: an outer local is visible inside the try, the catch arm and the
// finally, and again after the `end try`.

module M; class C public static method Main(): int var n := 3; try Console.WriteLine(n); catch Exception => Console.WriteLine(n), finally Console.WriteLine(n); end try; return n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
