//! case: s28_2_8413
//! doc: spec
//! section: 28.2
//! title: a lambda body's invocation of a closure over a discarded variable is rejected
//! rule: its invocation after the kill is a use of the variable under the first sentence
//! cites: 28.2, 27.5, 17.13
//! expect: reject
//! diagnostic: dispose
//! notes: Pins #9085 (closure form). `g` captures the variable `x` itself; `_ := x;` kills it;
//!   `g()` after the kill reads `x`. Twin: s28_2_8414 (the closure runs before the
//!   discard).
module Conf;
import System;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1); var g := lambda(): int => x.tag; _ := x; return g();
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine(C.F(true));
    return 0;
  end Main;
end C;
