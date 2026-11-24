//! case: s28_14_8413
//! doc: spec
//! section: 28.14
//! title: an overriding virtual indexer getter capturing its call-bounded parameter invokes it
//!   twice
//! rule: The caller side is correspondingly unrestricted: any closure — capture-bearing or not
//!   — may be passed to a call-bounded parameter
//! cites: 28.14, 23.4
//! expect: run
//! output: 17200
//! notes: Pins #9068's override shape (vtable dispatch from a base-typed receiver). `Fl2`'s
//!   getter captures `q` in the local closure `h` and returns `h() + h()` = 2 * (42 + 1) =
//!   86 per iteration; 200 * 86 = 17200.
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
public class Fl2 : Fl
  public override property [q: () -> int]: int
    get => do var h := lambda() => q() + 1; return h() + h(); end;
  end;
  public constructor() end constructor;
end Fl2;
public class Program
  public static method Main(): int
    var t := 0;
    for var j in 0..<200 do
      var r := new Res();
      var fl: Fl := new Fl2();
      t := t + fl[lambda() => r.V];
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
