//! case: s20_8001
//! doc: spec
//! section: 20
//! title: a self-referencing guarded expression-bodied function-typed property read yields
//!   the else lambda
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its
//!   branch, the `else` branch is selected when none holds, and only the selected branch
//!   is evaluated.
//! cites: 20, 23.2
//! expect: run
//! stdout: RESULT 2
//! notes: Pins the #8981 fix-forward (a recursion guard on the property-read closure
//!   predicate): the self-referencing guarded read must compile and run.
module Conf;
public class H
  private _d: int;
  public constructor() _d := 3; end;
  public property A: (int) -> int => (if _d > 5 then this.A else lambda(x: int) => x + 1);
end H;
public static class Program
  public static method Main(): int
    using var h := new H();
    var g := h.A;
    Console.WriteLine("RESULT " + g(1).ToString());
    return 0;
  end;
end Program;
