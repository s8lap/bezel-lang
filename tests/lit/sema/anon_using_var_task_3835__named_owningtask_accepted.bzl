// Issue #3835 / spec §17.13 (v0.288, axiom A6) / §30.10: the CONTROL for the
// sibling reject test (anon_using_var_task_3835__owningtask_rejected.bzl) —
// same program, only the binding is NAMED (`using var t := ...`), which is
// the unsupervised idiom §30.10 keeps legal for exactly this reason (a named
// binding CAN be discharged/awaited later; only the anonymous `_` spelling
// abandons it unawaitably).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static async method Make(): OwningTask<R> return new R(); end Make; public static method Main(): int using var t := Make(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
