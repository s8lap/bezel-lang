//! case: s28_2_4803
//! doc: spec
//! section: 28.2
//! title: a holder invoked after a finally block that disposes its object is rejected
//! rule: taken while the variable held that value, and every value in the holder's source set ([K2.2](#sec-kernel-k2)) has been so disposed on every path to the use, a use of the holder
//! cites: 28.3, 27.6
//! expect: reject
//! diagnostic: dominating explicit Dispose()
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
  public static method MayThrow(k: bool): int
    if k then
      throw new InvalidOperationException("x");
    end;
    return 1;
  end MayThrow;

  public static method Run(k: bool): int
    var d := new D(7);
    var f := d.Ping;
    var r := 0;
    try
      r := C.MayThrow(false);
    finally
      d.Dispose();
    end;
    return r + f();
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
