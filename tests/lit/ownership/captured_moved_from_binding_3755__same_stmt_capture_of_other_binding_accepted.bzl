// §28.3 / §22.5 (v0.286, issue #3755, review follow-up) ACCEPT control: the
// statement consumes `p`, but the closure captures a DIFFERENT binding `q`.
// Only captures OF THE CONSUMED BINDING are rejected — a blanket "this
// statement contains a move, reject every lambda in it" rule would fail here
// and would cost a large class of legal programs.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics. // per §28.3, §22.5 (v0.286)
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method Go(x: owned R, f: () -> int): int x.Dispose(); return f(); end Go; public static method Run(): int var p := new R(); using var q := new R(); return Go(move p, lambda(): int => q.V()); end Run; public static method Main(): int Console.WriteLine(Run().ToString()); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
