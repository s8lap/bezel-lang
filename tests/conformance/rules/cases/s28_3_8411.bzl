//! case: s28_3_8411
//! doc: spec
//! section: 28.3
//! title: a lambda that discards a captured owned binding of the enclosing body is rejected
//! rule: an explicit `Dispose()` or discard of it, and every other owned-consuming use of it
//!   are compile errors
//! cites: 28.3, 17.13, 27.5
//! expect: reject
//! diagnostic: captured
//! notes: Pins #9016's neighbour: inside a closure a captured owned binding is in borrow state,
//!   so the discard `_ := o` consumes a binding the closure does not own. Twin: s28_3_8412
//!   (the closure reads the captured binding and discards only its own local).
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
      if flag then _ := o; return 1; end;
      return 2;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine(C.F(true));
    return 0;
  end Main;
end C;
