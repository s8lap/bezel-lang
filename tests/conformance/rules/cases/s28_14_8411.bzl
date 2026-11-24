//! case: s28_14_8411
//! doc: spec
//! section: 28.14
//! title: a function parameter forwarded to a virtual indexer's call-bounded parameter is
//!   invoked
//! rule: The caller side is correspondingly unrestricted: any closure — capture-bearing or not
//!   — may be passed to a call-bounded parameter
//! cites: 28.14, 23.4
//! expect: run
//! stdout: 8400
//! notes: Pins #9068's parameter shape (the caller must not release a closure it does not own).
//!   `Use` forwards its own call-bounded parameter `g` to the virtual indexer, whose
//!   getter invokes it. Each of the 200 closures returns 42: 200 * 42 = 8400.
module Conf;
import System;
public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public class Fl
  public virtual property [q: () -> int]: int => q();
  public constructor() end constructor;
end Fl;
public class Program
  public static method Use(fl: Fl, g: () -> int): int return fl[g]; end Use;
  public static method Main(): int
    var t := 0;
    var fl := new Fl();
    for var j in 0..<200 do
      var r := new Res();
      t := t + Use(fl, lambda() => r.V);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
