//! case: s28_3_8_1301
//! doc: spec
//! section: 28.3.8
//! title: GetHashCode on a function-typed local from a ternary of two fields is admitted
//! rule: A function value as the receiver of a member `Object` declares (`f.ToString()`,
//!   `f.GetHashCode()`, an interpolation hole) is not at a set-erasing position
//! cites: 28.3, 27.7.1
//! expect: run
//! output: 900
module Conf;
class Holder
  public Fn: (int)->int;
  public Gn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Gn := lambda(x: int) => x + 9; end constructor;
  public method Get(c: bool): (int)->int => (if c then this.Fn else this.Gn);
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.Get(true);
    return g.GetHashCode() - g.GetHashCode() + 1;
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
