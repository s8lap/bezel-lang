// §28.3 per-path (v0.286, issue #3755, second review follow-up) ACCEPT
// control: the case-expression twin of the if-atom row. The consuming arm and
// the capturing arm are mutually exclusive, so the capture reads a live `p` on
// every path that reaches it. Confirms the arm scoping is structural and
// covers `CaseExpr` as well as `ConditionalExpr` — the gap was present in
// both, so a fix that handled only one would fail here.
//
// ACCEPT control: `bezelc -c` must exit zero and emit no error and no capture
// diagnostic (the unrelated K3.14 auto-drop warning is expected).
// // per §28.3, §19 (v0.286)
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method Consume(x: owned R): () -> int x.Dispose(); return lambda(): int => 99; end Consume; public static method Run(flag: bool): int var p := new R(); var f: () -> int := (case flag of true => Consume(move p), _ => lambda(): int => p.V()); return f(); end Run; public static method Main(): int Console.WriteLine(Run(false).ToString()); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: closure captures
