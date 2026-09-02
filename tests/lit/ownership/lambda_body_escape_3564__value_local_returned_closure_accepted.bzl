// §28.9 (v0.286), issue #3564 — ACCEPT control on the OTHER axis: the returned
// closure escapes the lambda body exactly as in the rejected row, but what it
// captures is a value-typed local, which carries no disposal obligation
// (§28.3: only the Owned state carries a scope-bound discharge obligation).
//
// The fix seeds the lambda body's `states_` from `localIsOwnedObligation`, so
// a plain `var q := 7;` is never seeded and the §28.9 leg never sees it. A
// seed that marked every lambda-body local Owned would reject this program.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
class C
  public static method Run(): int
    var mk := lambda(): () -> int => do
      var q := 7;
      return lambda(): int => q;
    end;
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
