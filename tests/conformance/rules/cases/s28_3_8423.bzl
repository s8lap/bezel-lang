//! case: s28_3_8423
//! doc: spec
//! section: 28.3
//! title: a capture-bearing closure passed at an Object parameter inside a property getter body
//!   is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9034 (the getter host). The block getter forms a closure capturing the owned
//!   local `reg` and passes it at `Keep(o: Object)`. Twin: s28_3_8426.
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
  public property P: int
    get => do
      var reg := new Reg2();
      return reg.Keep(lambda(): int => reg.Peek());
    end;
  end P;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine(fl.P);
    return 0;
  end Main;
end Program;
