//! case: s28_2_4809
//! doc: spec
//! section: 28.2
//! title: a holder re-bound to the new object's method after the first is disposed is invoked
//! rule: taken while the variable held that value, and every value in the holder's source set ([K2.2](#sec-kernel-k2)) has been so disposed on every path to the use, a use of the holder
//! cites: 28.3, 27.6
//! expect: run
//! output: 9
module Conf;

public class D
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;

  public method Ping(): int => _n;
  public static method Stat(): int => 5;
end D;

public class C
  public static method Run(k: bool): int
    var d := new D(7);
    var f := d.Ping;
    d.Dispose();
    d := new D(9);
    f := d.Ping;
    return f();
  end Run;

  public static method Main()
    Console.WriteLine(C.Run(false));
  end Main;
end C;
