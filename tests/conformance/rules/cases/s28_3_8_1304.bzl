//! case: s28_3_8_1304
//! doc: spec
//! section: 28.3.8
//! title: GetHashCode on a function-typed local from a block override returning a field is admitted
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
  public virtual method Get(): (int)->int => this.Fn;
end Holder;
class Sub: Holder
  public constructor() base(); end constructor;
  public override method Get(): (int)->int
    return this.Gn;
  end Get;
end Sub;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.Get();
    return g.GetHashCode() - g.GetHashCode() + 1;
  end Run;
  public static method Main(): int
    using var h := new Sub();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
