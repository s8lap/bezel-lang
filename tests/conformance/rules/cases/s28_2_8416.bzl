//! case: s28_2_8416
//! doc: spec
//! section: 28.2
//! title: a lambda body's read through an alias before a conditional disposal of its source is
//!   admitted
//! rule: a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3, 17.13
//! expect: run
//! stdout: 1
//! notes: Admitting twin of s28_2_8415 (#9085): the read precedes the if/else that disposes `x`
//!   on both arms. Prints 1.
module Conf;
import System;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1); var y := x; var n := y.tag; if flag then _ := x; else x.Dispose(); end; return n;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine(C.F(true));
    return 0;
  end Main;
end C;
