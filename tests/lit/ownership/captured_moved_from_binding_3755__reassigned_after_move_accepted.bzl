// §28.8.1 (v0.286, issue #3755) ACCEPT control: `p` is reassigned to a fresh
// value after the move, so the binding is Owned again at the lambda literal.
// §28.8.1: a moved-from binding is unreadable "until reassigned" — this row is
// the "until", and it is what makes the rule flow-sensitive rather than a
// blanket rejection of every post-move capture of the name.
//
// Reject control: bezelc -c must exit zero and emit no
// diagnostics. // per §28.8.1, §28.3 (v0.286)
//
// RUN: %not %bezelc -c -o %t.o %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class Box public method Run() var p := new R(); var q := move p; p := new R(); var f := lambda(): int => p.V(); _ := f(); _ := move q; p.Dispose(); end Run; end Box; class C public static method Main(): int using var box := new Box(); box.Run(); return 0; end Main; end C;


// CHECK: the `move` marker is redundant on a {{.*}} (§28.3 K3.13 marker economy; K3.9)