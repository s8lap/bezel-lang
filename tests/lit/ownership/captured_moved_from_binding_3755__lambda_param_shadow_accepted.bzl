// §28.3 / §27.5 (v0.286, issue #3755) ACCEPT control: the lambda declares its
// OWN parameter named `p`, shadowing an enclosing moved-from `p`. The inner
// `p` resolves to the lambda's parameter symbol, which is not a method-scope
// binding at all, so nothing is captured and nothing may be rejected.
//
// This is the row a NAME-keyed implementation of the rule would fail: the
// checker resolves captures BY SYMBOL (`ctx_.exprSymbol`), not by name.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics. // per §28.3, §27.5 (v0.286)
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class Box public method Run() var p := new R(); var q := move p; var f := lambda(p: int): int => p + 1; _ := f(41); _ := move q; end Run; end Box; class C public static method Main(): int using var box := new Box(); box.Run(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
