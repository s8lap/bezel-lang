//! case: s28_2_8415
//! doc: spec
//! section: 28.2
//! title: a lambda body's read through an alias after both arms disposed its source is rejected
//! rule: has been so disposed on every path to the use, a use of the holder
//! cites: 28.2, 28.3, 17.13
//! expect: reject
//! diagnostic: dispose
//! notes: Pins #9085 (join shape). Both arms of the `if` dispose `x` (the discard on one,
//!   `Dispose()` on the other), so on every path to `y.tag` the source is disposed. Twin:
//!   s28_2_8416 (the read precedes the conditional).
module Conf;
import System;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1); var y := x; if flag then _ := x; else x.Dispose(); end; return y.tag;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine(C.F(true));
    return 0;
  end Main;
end C;
