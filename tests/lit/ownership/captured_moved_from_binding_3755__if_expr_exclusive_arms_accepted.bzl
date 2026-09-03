// §28.3 per-path (v0.286, issue #3755, second review follow-up) ACCEPT
// control: the `move` and the capturing lambda literal sit in MUTUALLY
// EXCLUSIVE arms of the §20 if-atom. Only one arm ever runs, so on the `else`
// path `p` was never moved and the capture reads a definitely-live binding.
// §28.3's obligations are per-path — "the scope-end auto-drop applies
// per-path, disposal runs only on the still-owned path" — so pooling the two
// arms' effects into one flat per-statement set is wrong, and this program
// must compile and run.
//
// Statement-level control flow never had this problem (the captured-consume
// walker returns early for IfStmt/CaseStmt/... at the top level and each
// nested statement is checked separately); `ConditionalExpr` and `CaseExpr`
// are EXPRESSION nodes walked as part of one statement, and they are the only
// two expression nodes with mutually exclusive alternatives.
//
// ACCEPT control: `bezelc -c` must exit zero and emit no error and no
// capture diagnostic. (The §28.3 K3.14 auto-drop WARNING for the plain-`var`
// `p` on the non-consuming path is expected and unrelated — it is present on
// the identical lambda-free program too.) // per §28.3, §20 (v0.286)
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method Consume(x: owned R): () -> int x.Dispose(); return lambda(): int => 99; end Consume; public static method Run(flag: bool): int var p := new R(); var f: () -> int := (if flag then Consume(move p) else lambda(): int => p.V()); return f(); end Run; public static method Main(): int Console.WriteLine(Run(false).ToString()); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: closure captures
