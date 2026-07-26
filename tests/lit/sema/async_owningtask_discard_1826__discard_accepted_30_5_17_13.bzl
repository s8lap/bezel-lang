// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Regression for issue #1826 (spec §30.5 / §17.13, v0.196).
//
// §17.13: the explicit discard `_ := expression;` is an OWNED-CONSUMING
// position — when the operand is an OWNED value (an `owned` T, an
// unbound producer of a class/interface, an `OwningTask<T>`'s Pattern B
// awaited result), the discard disposes it at end-of-statement, exactly as
// `using var _ := expression;` does (with end-of-STATEMENT rather than
// end-of-SCOPE disposal timing).
//
// §30.5: "awaiting an `OwningTask<T>` (Pattern B) must appear in an
// owned-consuming context … a `using var`/`var` binding, an explicit
// `_ := ...` discard (§17.13), an `owned` argument, an owned-field/owned-
// slot assignment, or an owned `return`". The previous code missed the
// `_ := await t;` discard — `collectOwnedConsumingAwaitsStmt` had no
// `DiscardStmt` case, so the await fell off the `consuming` set and the
// Pattern B site check rejected the program.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.

module M;
import System.Threading;
class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;

class C
  public static async method MakeAsync(): OwningTask<Cell>
    return new Cell();
  end MakeAsync;

  public static async method Main(): Task<int>
    using var t := C.MakeAsync();
    _ := await t;   // §17.13 / §30.5: discard is an owned-consuming position
    return Cell.n;  // t's cell disposed at end of the discard statement
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
