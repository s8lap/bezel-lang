// §28.9 (v0.286), issue #3564 — ACCEPT control for the EXPRESSION-BODIED
// lambda path, and a regression this fix's own first draft introduced.
//
// `checkOneLambdaBodyEscapes` clears and re-seeds `states_` from the lambda
// body before running the §28.9 leg, which is what scopes the fix. An
// expression-bodied lambda (`=> E`) has no statements to seed FROM — so the
// clear must still happen, leaving the map empty, or the leg resolves the
// enclosing METHOD's locals and rejects this program.
//
// Measured on the fix's first draft, which returned early before the clear:
// REJECTED at 11:55 with the §28.9 diagnostic. The program is memory-safe —
// `g` is called while `r` is still in scope — so that was a false reject of a
// legal program, and whether the closure escapes the METHOD is the analysis
// this pre-pass does not have (issue #3702).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
class R
  public method V(): int
    return 5;
  end V;
end R;
class C
  public static method Run(): int
    using var r := new R();
    var mk := lambda(): () -> int => lambda(): int => r.V();
    var g := mk();
    return g() + g();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
