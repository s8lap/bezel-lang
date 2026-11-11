//! case: s28_2_4805
//! doc: spec
//! section: 28.2
//! title: a holder re-bound to a lambda after its object is disposed is invoked without error
//! rule: taken while the variable held that value, and every value in the holder's source set ([K2.2](#sec-kernel-k2)) has been so disposed on every path to the use, a use of the holder
//! cites: 28.3, 27.6
//! expect: run
//! output: 600
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
    f := lambda() => 3;
    return f();
  end Run;

  public static method Main()
    var total := 0;
    var i := 0;
    while i < 200 do
      total := total + C.Run(i >= 0);
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end C;
