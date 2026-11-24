//! case: s28_3_8412
//! doc: spec
//! section: 28.3
//! title: a lambda may read a captured owned binding while discarding an owned local of its own
//! rule: an explicit `Dispose()` or discard of it, and every other owned-consuming use of it
//!   are compile errors
//! cites: 28.3, 17.13, 27.5
//! expect: run
//! stdout: 1500
//! notes: Admitting twin of s28_3_8411 (#9016): the closure only reads `o.tag` (a borrow) and
//!   discards its own `x`. Even i: x discarded, returns 7; odd i: x disposed, returns 8;
//!   100*7 + 100*8 = 1500.
module Conf;
import System;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var o := new Res(7);
    var f := lambda(flag: bool): int => do
      var x := new Res(1);
      if flag then _ := x; return o.tag; end;
      x.Dispose();
      return o.tag + 1;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    var i := 0;
    var s := 0;
    while i < 200 do
      s := s + C.F(i mod 2 = 0);
      i := i + 1;
    end;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
