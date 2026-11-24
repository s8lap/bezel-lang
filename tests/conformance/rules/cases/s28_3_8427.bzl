//! case: s28_3_8427
//! doc: spec
//! section: 28.3
//! title: a lambda that only invokes its own function-typed parameter inside a setter body is
//!   admitted
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an
//!   argument to another call-bounded parameter
//! cites: 28.3, 28.14, 23.3
//! expect: run
//! stdout: 5
//! notes: Admitting twin of s28_3_8422 (#9034): the lambda's own parameter `q` is only invoked,
//!   never passed at an Object position. The setter stores h(lambda(): int => 5) = 5.
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
      var h := lambda(q: () -> int): int => q();
      this.Last := h(lambda(): int => 5);
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
