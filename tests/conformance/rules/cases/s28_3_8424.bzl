//! case: s28_3_8424
//! doc: spec
//! section: 28.3
//! title: a capture-bearing closure passed at an Object parameter inside an indexer setter body
//!   is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.4
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9034 (the indexer host): an indexer's block-form setter body is checked like a
//!   method body. Twin: s28_3_8426.
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
  public property [i: int]: int
    get => 0;
    set => do
      var reg := new Reg2();
      this.Last := reg.Keep(lambda(): int => reg.Peek());
    end;
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    fl[1] := 1;
    Console.WriteLine(fl.Last);
    return 0;
  end Main;
end Program;
