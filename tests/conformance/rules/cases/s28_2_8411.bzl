//! case: s28_2_8411
//! doc: spec
//! section: 28.2
//! title: a lambda body's read through an alias after the discard of its source is rejected
//! rule: has been so disposed on every path to the use, a use of the holder
//! cites: 28.2, 28.3, 17.13
//! expect: reject
//! diagnostic: dispose
//! notes: Pins #9085 (the lambda body runs the method body's holder check, §28.3.10's
//!   uniformity across body kinds). `y` borrows `x`'s value; `_ := x;` disposes it;
//!   `y.tag` is a use of the holder after every path disposed the source. Twin: s28_2_8412
//!   (the read precedes the discard).
module Conf;
import System;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1); var y := x; _ := x; return y.tag;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine(C.F(true));
    return 0;
  end Main;
end C;
