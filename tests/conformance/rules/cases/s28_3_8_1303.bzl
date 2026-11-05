//! case: s28_3_8_1303
//! doc: spec
//! section: 28.3.8
//! title: GetHashCode on a function-typed local from an interface method returning a field is admitted
//! rule: A function value as the receiver of a member `Object` declares (`f.ToString()`,
//!   `f.GetHashCode()`, an interpolation hole) is not at a set-erasing position
//! cites: 28.3, 27.7.1
//! expect: run
//! output: 100
module Conf;
interface IGet
  method Get(): (int)->int;
end IGet;
class Holder
  public Fn: (int)->int;
  public Gn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Gn := lambda(x: int) => x + 9; end constructor;
end Holder;
class Impl: IGet
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 5; end constructor;
  public method Get(): (int)->int => this.Fn;
end Impl;
class P
  public static method Run(h: IGet): int
    var g: (int)->int := h.Get();
    return g.GetHashCode() - g.GetHashCode() + 1;
  end Run;
  public static method Main(): int
    using var h := new Impl();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + 0; i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
