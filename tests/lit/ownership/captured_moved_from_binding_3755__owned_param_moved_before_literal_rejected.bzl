// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 *Captured owned bindings* / §27.5 / §28.8.1 (v0.286, issue #3755):
// the issue's own repro, `owned` PARAMETER form. `p` is moved into `q`, then
// captured by a closure stored in a field.
//
// §28.3: "Inside a closure, a captured owned binding — an owned local or
// `owned` parameter of the enclosing body — is uniformly in **borrow state**:
// reads borrow". §28.8.1: "a moved-from *binding* is simply unreadable until
// reassigned, with no restoration obligation." The capture is a read of an
// unreadable binding.
//
// Before the fix this compiled rc=0 with NO diagnostic and printed a garbage
// int read out of freed storage. The DIRECT read of the same binding
// (`p.V();` with no closure) was already rejected — the closure body was the
// hole. // per §28.3, §27.5, §28.8.1 (v0.286)

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class Box public h: (() -> int)?; public method Take(p: owned R) var q := move p; this.h := lambda(): int => p.V(); _ := move q; end Take; end Box; class C public static method Main(): int using var box := new Box(); box.Take(new R()); Console.WriteLine(box.h!().ToString()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure captures `p` whose owned value was already moved out at this point (§28.3)
// CHECK-SAME: the capture READS the binding
