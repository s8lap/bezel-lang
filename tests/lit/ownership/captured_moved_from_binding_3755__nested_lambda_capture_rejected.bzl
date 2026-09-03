// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 *Captured owned bindings* (v0.286, issue #3755): the moved-from
// binding is captured by an INNER closure declared inside an outer one, and
// only there. `p` is an enclosing-METHOD binding at both depths, so §28.3's
// classification applies unchanged and the read is equally illegal.
//
// Exactly ONE diagnostic is expected: a nested lambda's identifiers are
// reached by both its own and its enclosing lambda's free-variable walk, so
// the per-site de-duplication is what keeps this from double-reporting.
// // per §28.3, §27.5 (v0.286)

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class Box public h: (() -> int)?; public method Take() var p := new R(); var q := move p; this.h := lambda(): int => do var g := lambda(): int => p.V(); return g(); end; _ := move q; end Take; end Box; class C public static method Main(): int using var box := new Box(); box.Take(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure captures `p` whose owned value was already moved out at this point (§28.3)
// CHECK-NOT: closure captures `p` whose owned value was already moved out
