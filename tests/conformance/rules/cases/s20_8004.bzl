//! case: s20_8004
//! doc: spec
//! section: 20
//! title: a static function-typed property cycle guarded by a static property reads its else
//!   lambda
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its
//!   branch, the `else` branch is selected when none holds, and only the selected branch
//!   is evaluated.
//! cites: 20, 23.2
//! expect: run
//! stdout: 800
//! notes: Pins the #8981 fix-forward for the static spelling (the guard is keyed by the
//!   property symbol, not the receiver).
module Conf;
public class H
  public static property D: int => 3;
  public static property A: (int) -> int => (if H.D > 5 then H.B else lambda(x: int) => x + 1);
  public static property B: (int) -> int => (if H.D > 6 then H.A else lambda(x: int) => x + 2);
end H;
public static class Program
  public static method Main(): int
    var tot := 0;
    for var i in 0..<200 do
      var g := H.A;
      tot := tot + g(1);
      tot := tot + H.A(1);
    end;
    Console.WriteLine(tot);
    return 0;
  end;
end Program;
