//! case: s20_8002
//! doc: spec
//! section: 20
//! title: a mutually guarded pair of function-typed properties read 200 times sums the else
//!   lambdas
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its
//!   branch, the `else` branch is selected when none holds, and only the selected branch
//!   is evaluated.
//! cites: 20, 23.2
//! expect: run
//! stdout: 800
//! notes: Pins the #8981 fix-forward: a cyclic expression-bodied read inside a loop must not
//!   recurse in the compiler.
module Conf;
public class H
  private _d: int;
  public constructor() _d := 3; end;
  public property A: (int) -> int => (if _d > 5 then B else lambda(x: int) => x + 1);
  public property B: (int) -> int => (if _d > 6 then A else lambda(x: int) => x + 2);
end H;
public static class Program
  public static method Main(): int
    using var h := new H();
    var tot := 0;
    for var i in 0..<200 do
      var g := h.A;
      tot := tot + g(1);
      tot := tot + h.A(1);
    end;
    Console.WriteLine(tot);
    return 0;
  end;
end Program;
