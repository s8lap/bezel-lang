//! case: s28_3_8426
//! doc: spec
//! section: 28.3
//! title: a capture-free closure at an Object parameter inside a property setter is admitted
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.3
//! expect: run
//! stdout: 41
//! notes: Admitting twin of s28_3_8421..8425 (#9034). The lambda captures nothing, so its
//!   source set is empty and it is admitted at the set-erasing position; `Keep` invokes it
//!   and the setter stores 41 in `Last`.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Peek(): int return 5; end Peek;
  public method Keep(o: Object): int
    if o is () -> int then return (o as () -> int)!(); end if;
    return 0;
  end Keep;
  public method KeepT<T>(t: T): int return 0; end KeepT;
end Reg2;
public class Fl
  public Last: int;
  public constructor() end constructor;
  public property P: int
    get => 0;
    set => do
      var reg := new Reg2();
      this.Last := reg.Keep(lambda(): int => 41);
    end;
  end P;
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    fl.P := 1;
    Console.WriteLine(fl.Last);
    return 0;
  end Main;
end Program;
