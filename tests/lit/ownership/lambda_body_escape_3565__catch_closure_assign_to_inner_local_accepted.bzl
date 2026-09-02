// §31.1 (v0.286), issue #3565 — ACCEPT control, the exact boundary the fix
// must not cross.
//
// The channel fix #3565 reaches is a DEPTH COMPARISON, not a blanket reject:
// `checkBorrowStoreEscape` flags an assignment only when the TARGET local's
// declaring block strictly encloses the source's. Here `n` is declared INSIDE
// the catch arm, so it dies with the arm and never outlives the exception —
// §31.1's "a closure that MAY outlive the arm" is not met, and the program is
// legal. It must still compile clean now that the lambda body's own
// `localDepth_` is seeded (a seed that recorded the wrong depths, or a driver
// that skipped the comparison, would reject this).
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
    var mk := lambda(): int => do
      var total := 0;
      try
        throw new InvalidOperationException("wxyz");
      catch
        var e: Exception => do
          var n := lambda(): int => 0;
          n := lambda(): int => e.Message.Length;
          total := n() + n();
        end
      end try;
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
