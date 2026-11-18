//! case: s28_3_8_7604
//! doc: spec
//! section: 28.3.8
//! title: a borrow-capturing assignment after the read reaches it over the loop back edge and is rejected
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: reject
//! diagnostic: captured borrow
module Conf;
import System;
public class Res
  public V: int;
  public constructor() V := 42; end constructor;
end Res;
public class Reg
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method RunInt(): int => (_o as () -> int)!();
end Reg;
public class Program
  public static method Mk(reg: Reg, r: Res, c: bool)
    var n := 1;
    var f := lambda() => n;
    for var j in 0 ..< 2 do
      reg.Keep(f);
      f := lambda() => r.V;
    end for;
  end Mk;
  public static method Main(): int
    using var reg := new Reg();
    for var i in 0 ..< 3 do
      do
        var r := new Res();
        Mk(reg, r, i mod 2 = 0);
      end;
      Console.WriteLine(reg.RunInt());
    end for;
    return 0;
  end Main;
end Program;
