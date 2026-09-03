// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 / §22.5 (v0.286, issue #3755, second review follow-up): the REJECTING
// side of the per-arm scoping. The `move` sits in the ARGUMENT LIST, OUTSIDE
// the if-atom, so it is unconditional relative to the branch and co-executes
// with EVERY arm — including the one holding the capturing lambda. Scoping the
// arms must not lose this rejection.
//
// The companion accept rows put the consume INSIDE a sibling arm; this row is
// what distinguishes "different arms" from "merely inside some branch".
// // per §28.3, §22.5 (v0.286)

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method Go(x: owned R, f: () -> int): int x.Dispose(); return f(); end Go; public static method Run(flag: bool): int var p := new R(); return Go(move p, (if flag then lambda(): int => p.V() else lambda(): int => 0)); end Run; public static method Main(): int Console.WriteLine(Run(true).ToString()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure captures `p` whose owned value is transferred away by this same statement (§28.3 / §22.5)
