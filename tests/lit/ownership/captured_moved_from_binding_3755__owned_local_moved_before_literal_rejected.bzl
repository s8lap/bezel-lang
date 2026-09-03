// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 *Captured owned bindings* / §28.8.1 (v0.286, issue #3755): the plain
// owned LOCAL twin of the parameter repro. Measured byte-for-byte identical on
// the parent (compiled rc=0, printed a different garbage int), which is what
// shows the defect is NOT on the parameter/local axis fix #3746 closed — it is
// orthogonal to it and predates it. // per §28.3, §28.8.1 (v0.286)

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class Box public h: (() -> int)?; public method Take() var p := new R(); var q := move p; this.h := lambda(): int => p.V(); _ := move q; end Take; end Box; class C public static method Main(): int using var box := new Box(); box.Take(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure captures `p` whose owned value was already moved out at this point (§28.3)
// CHECK-SAME: unreadable until reassigned (§28.8.1)
