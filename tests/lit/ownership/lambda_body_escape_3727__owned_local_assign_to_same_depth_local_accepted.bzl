// §28.3 "Borrow lifetime rules" (v0.286), issue #3727 — the ACCEPT control
// that bounds the fix, not decoration.
//
// Byte-for-byte the reject row's body with ONE change: `f` is declared inside
// the SAME `do ... end` block as `r` rather than at method scope. The depths
// are then equal, `etd < esd` is false, and the new branch declines — the
// pre-existing, unmodified `else { return false; }` accepts. Genuinely safe,
// not merely unflagged: `f` and `r` die at the same block end, so no call of
// `f` can be sequenced after `r`'s disposal.
//
// This is the row that proves the fix did not become "any store to a target
// outside the checked lambda body is rejected" — the failure mode #3707's
// review found in its own first draft, guarded here by requiring a POSITIVE
// depth hit for BOTH operands in the enclosing frame rather than reading a
// missing depth as proof of anything.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class C
  public static method Run(): int
    var total := 0;
    do
      var f := lambda(): int => 0;
      using var r := new R();
      var mk := lambda(): int => do
        f := lambda(): int => r.V();
        return 0;
      end;
      total := mk() + f() + f();
    end;
    return total;
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
