// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #7831 (stdlib §4.4, v0.355) CONTROL: the DECLARED member keeps
// working. `Array<T>` DOES declare `IndexOf(item: T, startIndex: int := 0):
// int` (stdlib §4.4), so the call is admitted and the program compiles clean.
//
// This is the over-rejection guard: the #7831 fix refuses `IndexOf` on a
// concrete collection head whose stdlib section declares it, so a fix that
// read the stdlib's surface table backwards (or refused the name on every
// head) would reject this program.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.

module M;
import System;
class P
  public static method Main(): int
    const a: Array<int> := [1, 2, 1];
    Console.WriteLine(a.IndexOf(1));
    return 0;
  end Main;
end P;

// NODIAG-NOT: error
// NODIAG-NOT: §
