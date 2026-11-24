//! case: s28_3_8421
//! doc: spec
//! section: 28.3
//! title: a capture-bearing closure passed at an Object parameter inside a property setter body
//!   is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9034: a block-form setter body is a body for the set-erasing check, as a method
//!   body is. The lambda captures the owned local `reg` (non-empty source set) and reaches
//!   `Keep(o: Object)`. Twin: s28_3_8426 (a capture-free lambda at the same position).
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
      this.Last := reg.Keep(lambda(): int => reg.Peek());
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
