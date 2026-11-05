//! case: s28_3_8_1305
//! doc: spec
//! section: 28.3.8
//! title: GetHashCode on a function-typed local from a generic Box<(int)->int> getter is admitted
//! rule: A function value as the receiver of a member `Object` declares (`f.ToString()`,
//!   `f.GetHashCode()`, an interpolation hole) is not at a set-erasing position
//! cites: 28.3, 27.7.1
//! expect: run
//! output: 900
module Conf;
class Box<T>
  public Val: T;
  public constructor(v: T) this.Val := v; end constructor;
  public method Get(): T => this.Val;
end Box;
class P
  public static method Run(h: Box<(int)->int>): int
    var g: (int)->int := h.Get();
    return g.GetHashCode() - g.GetHashCode() + 1;
  end Run;
  public static method Main(): int
    using var h := new Box<(int)->int>(lambda(x: int) => x + 7);
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Val(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
