//! case: s28_2_8414
//! doc: spec
//! section: 28.2
//! title: a lambda body's invocation of a closure over a variable before its discard is
//!   admitted
//! rule: a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 27.5, 17.13
//! expect: run
//! stdout: 1
//! notes: Admitting twin of s28_2_8413 (#9085): `g()` runs before the discard; the kill follows
//!   the last use. Prints 1.
module Conf;
import System;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1); var g := lambda(): int => x.tag; var n := g(); _ := x; return n;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine(C.F(true));
    return 0;
  end Main;
end C;
