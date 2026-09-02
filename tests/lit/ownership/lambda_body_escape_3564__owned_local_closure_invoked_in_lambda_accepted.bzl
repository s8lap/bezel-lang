// §28.9 (v0.286), issue #3564 — ACCEPT control. §28.9's rule is about a
// closure that ESCAPES ("...and may not outlive that scope"); a closure that
// captures the same owned local and is invoked in place, inside the block
// that owns it, is the legal shape the rule exists to leave alone. §28.3
// states it directly: "Capture is permitted for closures that are
// immediately invoked".
//
// This is the direct twin of the rejected row: same `using var r`, same
// capture, only the escape removed. It guards the fix's blast radius — a
// lambda-body seed that fired on CAPTURE rather than on the return path would
// reject this.
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
    var mk := lambda(): int => do
      var total := 0;
      do
        using var r := new R();
        var f := lambda(): int => r.V();
        total := f() + f();
      end;
      return total;
    end;
    return mk();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
