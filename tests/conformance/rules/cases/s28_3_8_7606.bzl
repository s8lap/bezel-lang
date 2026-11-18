//! case: s28_3_8_7606
//! doc: spec
//! section: 28.3.8
//! title: a lambda capturing only a value-type local carries no borrow and is admitted at Object
//! rule: a captured value that carries no borrow
//! cites: 28.3
//! expect: run
//! stdout: 2
module Conf;
import System;
public class Reg
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method RunInt(): int => (_o as () -> int)!();
end Reg;
public class Program
  public static method Mk(reg: Reg)
    var n := 1;
    var f := lambda() => do n := n + 1; return n; end;
    reg.Keep(f);
  end Mk;
  public static method Main(): int
    using var reg := new Reg();
    Mk(reg);
    Console.WriteLine(reg.RunInt());
    return 0;
  end Main;
end Program;
