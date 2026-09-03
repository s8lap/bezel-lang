// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 *Captured owned bindings* / §22.5 *Evaluation order* (v0.286, issue
// #3755, review follow-up): the `move` and the capturing lambda literal are
// arguments of the SAME call. §22.5: "A call's supplied argument expressions
// are evaluated in call-site textual order — left to right as written, for
// positional and named arguments alike", so `p` is already moved-from where
// the literal is evaluated — but the check consulted the STATEMENT-ENTRY
// states and never saw it.
//
// Measured on the first landing of this fix (e367a45): compiled rc=0, printed
// a garbage int, and `--sanitize=address` reported
// `heap-use-after-free ... READ of size 4`. // per §28.3, §22.5 (v0.286)

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method Go(x: owned R, f: () -> int): int x.Dispose(); return f(); end Go; public static method Run(): int var p := new R(); return Go(move p, lambda(): int => p.V()); end Run; public static method Main(): int Console.WriteLine(Run().ToString()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure captures `p` whose owned value is transferred away by this same statement (§28.3 / §22.5)
// CHECK-SAME: nothing at the call site orders the closure's INVOCATION against that transfer
