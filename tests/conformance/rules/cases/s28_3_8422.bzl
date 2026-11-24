//! case: s28_3_8422
//! doc: spec
//! section: 28.3
//! title: a lambda's own function-typed parameter at an Object parameter in a setter is
//!   rejected
//! rule: A **plain function-typed parameter** of the enclosing body contributes itself
//! cites: 28.3, 23.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9034 (provenance clause 3 in an accessor body): `q` is a plain function-typed
//!   parameter of the lambda, so it contributes itself and its value is bounded by the
//!   call; handing it to `Keep(o: Object)` (a set-erasing position) is rejected. Twin:
//!   s28_3_8427 (the lambda only invokes `q`).
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
      var h := lambda(q: () -> int): int => reg.Keep(q);
      this.Last := h(lambda(): int => 1);
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
