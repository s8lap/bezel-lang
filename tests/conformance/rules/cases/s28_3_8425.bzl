//! case: s28_3_8425
//! doc: spec
//! section: 28.3
//! title: a capturing closure at a type parameter instantiated at Object in a setter is
//!   rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9034 (the bare-type-parameter spelling of the set-erasing position).
//!   `KeepT<Object>` declares its parameter at a bare type parameter substituted by
//!   `Object`, a set-erasing position; the closure captures `reg`. Twin: s28_3_8426.
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
      this.Last := reg.KeepT<Object>(lambda(): int => reg.Peek());
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
