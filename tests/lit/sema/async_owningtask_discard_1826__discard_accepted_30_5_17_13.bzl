// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s
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
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — the `Cell.n` counter field below
// is a mutable static of primitive type, which §22.12 rule 2 form (iii)
// (#1393 warned-admission ruling, v0.221) admits only with a
// declaration-site warning; unrelated to this test's own #1826 subject
// (the discard-disposal timing), so only pinned here to keep the ACCEPT
// assertion accurate rather than silently stale.

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

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `n` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
